#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "allocator.h"

namespace mooncake {

// The master owns this registry. Retiring a handle prevents new references;
// existing references keep its range alive until the last transfer completes.
class UbsMemoryAllocationRegistry {
   public:
    struct Allocation {
        AllocatedBuffer::Descriptor descriptor;
        std::function<void()> release;
        ~Allocation() {
            if (release) release();
        }
    };

    std::shared_ptr<Allocation> add(AllocatedBuffer::Descriptor descriptor,
                                    std::function<void()> release) {
        auto allocation = std::make_shared<Allocation>();
        allocation->release = std::move(release);
        std::lock_guard lock(mutex_);
        if (!next_id_)
            throw std::overflow_error("UBS allocation IDs exhausted");
        descriptor.ubsmem_allocation_id_ = next_id_++;
        allocation->descriptor = std::move(descriptor);
        allocations_.emplace(allocation->descriptor.ubsmem_allocation_id_,
                             allocation);
        return allocation;
    }

    std::shared_ptr<Allocation> pin(
        const AllocatedBuffer::Descriptor& descriptor) {
        std::lock_guard lock(mutex_);
        auto it = allocations_.find(descriptor.ubsmem_allocation_id_);
        if (it == allocations_.end()) return nullptr;
        auto allocation = it->second.lock();
        if (!allocation) return nullptr;
        const auto& current = allocation->descriptor;
        if (descriptor.protocol_ != "ubsmem" ||
            descriptor.buffer_address_ != current.buffer_address_ ||
            descriptor.size_ != current.size_ ||
            descriptor.transport_endpoint_ != current.transport_endpoint_) {
            return nullptr;
        }
        return allocation;
    }

    void retire(uint64_t id) {
        std::lock_guard lock(mutex_);
        allocations_.erase(id);
    }

   private:
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, std::weak_ptr<Allocation>> allocations_;
};

// Completed operation IDs are merged into intervals, so normal sequential
// transfers leave one small tombstone per client, not one per transfer.
class UbsMemoryTransferRegistry {
   public:
    explicit UbsMemoryTransferRegistry(
        std::shared_ptr<UbsMemoryAllocationRegistry> allocations)
        : allocations_(std::move(allocations)) {}

    bool begin(const UUID& client, uint64_t operation,
               const AllocatedBuffer::Descriptor& descriptor) {
        if (!operation) return false;
        std::lock_guard lock(mutex_);
        auto& state = clients_[client];
        auto next = state.ended.upper_bound(operation);
        if (next != state.ended.begin() && std::prev(next)->second >= operation)
            return false;
        auto active = state.active.find(operation);
        if (active != state.active.end()) {
            const auto& current = active->second->descriptor;
            return current.ubsmem_allocation_id_ ==
                       descriptor.ubsmem_allocation_id_ &&
                   current.buffer_address_ == descriptor.buffer_address_ &&
                   current.size_ == descriptor.size_ &&
                   current.protocol_ == descriptor.protocol_ &&
                   current.transport_endpoint_ ==
                       descriptor.transport_endpoint_;
        }
        auto allocation = allocations_->pin(descriptor);
        if (!allocation) return false;
        state.active.emplace(operation, std::move(allocation));
        return true;
    }

    void end(const UUID& client, uint64_t operation) {
        if (!operation) return;
        std::lock_guard lock(mutex_);
        auto& state = clients_[client];
        auto [it, inserted] = state.ended.emplace(operation, operation);
        if (it != state.ended.begin()) {
            auto previous = std::prev(it);
            if (previous->second >= operation - 1) {
                previous->second = std::max(previous->second, it->second);
                state.ended.erase(it);
                it = previous;
            }
        }
        auto next = std::next(it);
        if (next != state.ended.end() &&
            (it->second == UINT64_MAX || next->first <= it->second + 1)) {
            it->second = std::max(it->second, next->second);
            state.ended.erase(next);
        }
        state.active.erase(operation);
    }

   private:
    struct ClientState {
        std::map<uint64_t,
                 std::shared_ptr<UbsMemoryAllocationRegistry::Allocation>>
            active;
        std::map<uint64_t, uint64_t> ended;
    };
    std::shared_ptr<UbsMemoryAllocationRegistry> allocations_;
    std::mutex mutex_;
    std::map<UUID, ClientState> clients_;
};

class UbsMemoryBufferAllocator final
    : public BufferAllocatorBase,
      public std::enable_shared_from_this<UbsMemoryBufferAllocator> {
   public:
    UbsMemoryBufferAllocator(
        std::string name, uintptr_t base, size_t capacity, std::string endpoint,
        std::shared_ptr<UbsMemoryAllocationRegistry> registry =
            std::make_shared<UbsMemoryAllocationRegistry>())
        : name_(std::move(name)),
          endpoint_(std::move(endpoint)),
          base_(base),
          capacity_(capacity),
          registry_(std::move(registry)) {
        if (!base || !capacity || capacity > UINTPTR_MAX - base || !registry_) {
            throw std::invalid_argument("Invalid UBS pool range");
        }
        free_.emplace(0, capacity);
    }

    // Page granularity keeps each allocation's cache lines and ownership
    // pages apart from other keys.
    static constexpr size_t kGranularity = 4096;

    std::unique_ptr<AllocatedBuffer> allocate(size_t length) override {
        if (!length || length > SIZE_MAX - (kGranularity - 1)) return nullptr;
        const size_t reserved =
            (length + kGranularity - 1) & ~(kGranularity - 1);
        size_t offset;
        {
            std::lock_guard lock(mutex_);
            auto it = free_.begin();
            while (it != free_.end() && it->second < reserved) ++it;
            if (it == free_.end()) return nullptr;
            offset = it->first;
            const size_t remaining = it->second - reserved;
            if (remaining) free_.emplace(offset + reserved, remaining);
            free_.erase(it);
            RecordAllocation(reserved);
        }
        auto self = shared_from_this();
        // Construct outside mutex_: the lifetime callback takes that mutex.
        auto allocation = registry_->add(
            {length, base_ + offset, "ubsmem", endpoint_},
            [self, offset, reserved] { self->releaseRange(offset, reserved); });
        try {
            return std::make_unique<AllocatedBuffer>(
                self, allocation->descriptor, allocation);
        } catch (...) {
            registry_->retire(allocation->descriptor.ubsmem_allocation_id_);
            throw;
        }
    }

    void deallocate(AllocatedBuffer* handle) override {
        registry_->retire(handle->get_descriptor().ubsmem_allocation_id_);
    }

    size_t capacity() const override { return capacity_; }
    size_t size() const override { return GetUsageBytes(); }
    size_t getLargestFreeRegion() const override {
        std::lock_guard lock(mutex_);
        size_t largest = 0;
        for (const auto& [offset, length] : free_) {
            largest = std::max(largest, length);
        }
        return largest;
    }
    std::string getSegmentName() const override { return name_; }
    std::string getTransportEndpoint() const override { return endpoint_; }

   private:
    void releaseRange(size_t offset, size_t length) {
        std::lock_guard lock(mutex_);
        const size_t released = length;
        auto next = free_.lower_bound(offset);
        if (next != free_.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                offset = previous->first;
                length += previous->second;
                free_.erase(previous);
            }
        }
        if (next != free_.end() && offset + length == next->first) {
            length += next->second;
            free_.erase(next);
        }
        free_.emplace(offset, length);
        // Accounting uses the returned range only, not its free neighbours.
        RecordDeallocation(released);
    }

    std::string name_;
    std::string endpoint_;
    uintptr_t base_;
    size_t capacity_;
    std::shared_ptr<UbsMemoryAllocationRegistry> registry_;
    mutable std::mutex mutex_;
    std::map<size_t, size_t> free_;
};
}  // namespace mooncake
