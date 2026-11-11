#include "transport/ubsmem_transport/ubsmem_transport.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

#include "common.h"
#include "error.h"

namespace mooncake {
namespace {
// Reads a positive integer setting no larger than limit.
size_t readSetting(const char* name, size_t fallback, size_t limit) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    size_t used = 0;
    const auto result = std::stoll(value, &used);
    if (used != std::string(value).size() || result <= 0 ||
        static_cast<unsigned long long>(result) > limit) {
        throw std::invalid_argument(std::string("Invalid ") + name);
    }
    return static_cast<size_t>(result);
}
}  // namespace

UbsMemoryTransport::UbsMemoryTransport() {
    timeout_ = std::chrono::milliseconds(
        readSetting("MC_UBSMEM_TIMEOUT_MS", timeout_.count(), 3600000));
    chunk_size_ =
        readSetting("MC_UBSMEM_CHUNK_SIZE", chunk_size_, size_t{1} << 40);
    // Chunks own whole pages, so concurrent copies never share one.
    if (chunk_size_ % static_cast<size_t>(sysconf(_SC_PAGESIZE)) != 0) {
        throw std::invalid_argument(
            "MC_UBSMEM_CHUNK_SIZE must be a multiple of the page size");
    }
    worker_count_ = readSetting("MC_UBSMEM_WORKERS", worker_count_, 64);
}

UbsMemoryTransport::~UbsMemoryTransport() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    space_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

Status UbsMemoryTransport::freeBatchID(BatchID batch) {
    std::lock_guard lock(mutex_);
    if (uncertain_batches_.contains(batch)) {
        return Status::Context("UBS ownership is uncertain");
    }
    return Transport::freeBatchID(batch);
}

int UbsMemoryTransport::install(std::string& name,
                                std::shared_ptr<TransferMetadata> metadata,
                                std::shared_ptr<Topology> topology) {
    Transport::install(name, metadata, topology);
    auto desc = std::make_shared<SegmentDesc>();
    desc->name = name;
    desc->protocol = "ubsmem";
    metadata_->addLocalSegment(LOCAL_SEGMENT_ID, name, std::move(desc));
    const int result = metadata_->updateLocalSegmentDesc();
    if (result != 0) return result;
    for (size_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back(&UbsMemoryTransport::run, this);
    }
    return 0;
}

int UbsMemoryTransport::registerLocalMemory(void* addr, size_t length,
                                            const std::string& location,
                                            bool remote, bool update) {
    if (!addr || !length ||
        length > std::numeric_limits<uintptr_t>::max() -
                     reinterpret_cast<uintptr_t>(addr)) {
        return ERR_INVALID_ARGUMENT;
    }
    auto arena = remote ? UbsMemoryArena::findOwned(addr, length) : nullptr;
    if ((remote && !arena) ||
        (!remote && UbsMemoryArena::overlaps(addr, length))) {
        return ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(mutex_);
    const auto address = reinterpret_cast<uintptr_t>(addr);
    if (registrations_.contains(address)) return ERR_ADDRESS_OVERLAPPED;
    auto registration =
        std::make_shared<Registration>(length, arena, BufferDesc{});
    if (remote) {
        BufferDesc buffer{};
        buffer.name = location;
        buffer.addr = address;
        buffer.length = length;
        buffer.ubsmem = arena->descriptor();
        buffer.ubsmem.object_offset =
            address - reinterpret_cast<uintptr_t>(arena->base());
#ifdef ENABLE_MULTI_PROTOCOL
        buffer.protocol = "ubsmem";
#endif
        const int result = metadata_->addLocalMemoryBuffer(buffer, update);
        if (result != 0) {
            metadata_->removeLocalMemoryBuffer(addr, false);
            return result;
        }
        registration->descriptor = buffer;
    }
    registrations_.emplace(address, std::move(registration));
    return 0;
}

int UbsMemoryTransport::unregisterLocalMemory(void* addr, bool update) {
    std::lock_guard lock(mutex_);
    auto it = registrations_.find(reinterpret_cast<uintptr_t>(addr));
    if (it == registrations_.end()) return ERR_ADDRESS_NOT_REGISTERED;
    // The caller still owns staging memory until every queued copy is done.
    if (it->second.use_count() != 1) return ERR_INVALID_ARGUMENT;
    if (it->second->arena) {
        int result = metadata_->removeLocalMemoryBuffer(addr, false);
        if (result != 0) return result;
        if (update) {
            result = metadata_->updateLocalSegmentDesc();
            if (result != 0) {
                metadata_->addLocalMemoryBuffer(it->second->descriptor, false);
                return result;
            }
        }
    }
    registrations_.erase(it);
    return 0;
}

int UbsMemoryTransport::registerLocalMemoryBatch(
    const std::vector<BufferEntry>& buffers, const std::string& location) {
    std::vector<void*> registered;
    for (const auto& buffer : buffers) {
        const int result = registerLocalMemory(buffer.addr, buffer.length,
                                               location, true, false);
        if (result != 0) {
            for (void* addr : registered) unregisterLocalMemory(addr, false);
            return result;
        }
        registered.push_back(buffer.addr);
    }
    const int result = metadata_->updateLocalSegmentDesc();
    if (result != 0) {
        for (void* addr : registered) unregisterLocalMemory(addr, false);
    }
    return result;
}

int UbsMemoryTransport::unregisterLocalMemoryBatch(
    const std::vector<void*>& addresses) {
    for (void* addr : addresses) {
        const int result = unregisterLocalMemory(addr, true);
        if (result != 0 && result != ERR_ADDRESS_NOT_REGISTERED) return result;
    }
    return metadata_->updateLocalSegmentDesc();
}

bool UbsMemoryTransport::prepare(const TransferRequest& request, Work& work) {
    if (!request.source || !request.length ||
        (request.opcode != TransferRequest::READ &&
         request.opcode != TransferRequest::WRITE)) {
        return false;
    }
    const auto source = reinterpret_cast<uintptr_t>(request.source);
    auto local = registrations_.upper_bound(source);
    if (local == registrations_.begin()) return false;
    --local;
    const auto relative = source - local->first;
    if (local->second->arena || relative >= local->second->length ||
        request.length > local->second->length - relative) {
        return false;
    }
    work.source = local->second;
    const auto segment = metadata_->getSegmentDescByID(request.target_id);
    if (!segment || segment->protocol != "ubsmem") return false;
    for (const auto& buffer : segment->buffers) {
        if (request.target_offset < buffer.addr) continue;
        const auto offset = request.target_offset - buffer.addr;
        if (offset >= buffer.length) continue;
        const auto& descriptor = buffer.ubsmem;
        if (request.length > buffer.length - offset || !descriptor.valid() ||
            buffer.length > descriptor.object_size - descriptor.object_offset) {
            return false;
        }
        work.arena = UbsMemoryArena::import(descriptor);
        imports_[descriptor.object_name] = work.arena;
        work.offset = descriptor.object_offset + offset;
        return true;
    }
    return false;
}

Status UbsMemoryTransport::submitTransferTask(
    const std::vector<TransferTask*>& tasks) {
    for (auto* task : tasks) {
        const auto& request = *task->request;
        task->total_bytes = request.length;
        Work target{nullptr, nullptr, nullptr, 0,
                    std::chrono::steady_clock::now() + timeout_};
        std::unique_lock lock(mutex_);
        bool prepared = false;
        try {
            prepared = !stopping_ && prepare(request, target);
        } catch (const std::exception& error) {
            LOG(ERROR) << "UBS transfer preparation failed: " << error.what();
        }
        // Chunks end at multiples of the chunk size within the object, so
        // chunks of one request never share a page. A request that cannot
        // be prepared gets one slice that fails.
        std::vector<Work> chunks;
        size_t done = 0;
        do {
            size_t length = request.length - done;
            if (prepared) {
                const size_t offset = target.offset + done;
                length = std::min(length, chunk_size_ - offset % chunk_size_);
            }
            auto* slice = getSliceCache().allocate();
            slice->source_addr = static_cast<char*>(request.source) + done;
            slice->length = length;
            slice->opcode = request.opcode;
            slice->target_id = request.target_id;
            slice->task = task;
            slice->status = Slice::PENDING;
            task->slice_list.push_back(slice);
            Work chunk = target;
            chunk.slice = slice;
            chunk.offset = target.offset + done;
            chunks.push_back(std::move(chunk));
            done += length;
        } while (prepared && done < request.length);
        // Publish the count before any chunk can complete.
        __atomic_store_n(&task->slice_count, chunks.size(), __ATOMIC_RELEASE);
        size_t queued = 0;
        while (prepared && queued < chunks.size()) {
            // A full queue applies backpressure until the request's deadline.
            const bool has_space = space_.wait_until(
                lock, target.deadline,
                [this] { return stopping_ || queue_.size() < kQueueCapacity; });
            if (!has_space || stopping_) break;
            queue_.push_back(std::move(chunks[queued++]));
            ready_.notify_one();
        }
        for (size_t i = queued; i < chunks.size(); ++i) {
            chunks[i].slice->markFailed();
        }
    }
    return Status::OK();
}

void UbsMemoryTransport::run() {
    for (;;) {
        Work work;
        bool stopping;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            work = std::move(queue_.front());
            queue_.pop_front();
            stopping = stopping_;
        }
        space_.notify_one();
        bool success = false;
        bool uncertain = false;
        try {
            success =
                !stopping &&
                work.arena->copy(work.offset, work.slice->source_addr,
                                 work.slice->length,
                                 work.slice->opcode == TransferRequest::WRITE,
                                 work.deadline);
        } catch (const std::exception& error) {
            uncertain = true;
            LOG(ERROR) << "UBS copy failed: " << error.what();
        }
        // Release pins before publishing completion: callers may immediately
        // unregister their buffers or destroy the batch on another thread.
        auto* slice = work.slice;
        work.source.reset();
        work.arena.reset();
        std::lock_guard lock(mutex_);
        if (uncertain) uncertain_batches_.insert(slice->task->batch_id);
        if (success) {
            slice->markSuccess();
        } else {
            slice->markFailed();
        }
    }
}

Status UbsMemoryTransport::submitTransfer(
    BatchID batch, const std::vector<TransferRequest>& requests) {
    auto& desc = toBatchDesc(batch);
    if (requests.size() > desc.batch_size - desc.task_list.size()) {
        return Status::TooManyRequests("UBS batch capacity exceeded");
    }
    std::vector<TransferTask*> tasks;
    for (const auto& request : requests) {
        auto& task = desc.task_list.emplace_back();
        task.batch_id = batch;
        task.request = &request;
        task.transport_ = this;
        tasks.push_back(&task);
    }
    return submitTransferTask(tasks);
}

Status UbsMemoryTransport::getTransferStatus(BatchID batch, size_t index,
                                             TransferStatus& status) {
    // Pollers cannot free the batch while markSuccess/markFailed is still
    // publishing event-driven completion after updating the slice counters.
    std::lock_guard lock(mutex_);
    auto& desc = toBatchDesc(batch);
    if (index >= desc.task_list.size()) {
        return Status::InvalidArgument("Invalid UBS task index");
    }
    if (uncertain_batches_.contains(batch)) {
        // Drain all slices before reporting uncertainty. Otherwise a caller
        // could release staging buffers while another slice still uses them.
        for (const auto& pending : desc.task_list) {
            const auto done =
                __atomic_load_n(&pending.failed_slice_count, __ATOMIC_ACQUIRE) +
                __atomic_load_n(&pending.success_slice_count, __ATOMIC_ACQUIRE);
            if (done !=
                __atomic_load_n(&pending.slice_count, __ATOMIC_ACQUIRE)) {
                status.s = PENDING;
                status.transferred_bytes = 0;
                return Status::OK();
            }
        }
        return Status::Context("UBS ownership is uncertain; retain allocation");
    }
    auto& task = desc.task_list[index];
    const auto failed =
        __atomic_load_n(&task.failed_slice_count, __ATOMIC_ACQUIRE);
    const auto success =
        __atomic_load_n(&task.success_slice_count, __ATOMIC_ACQUIRE);
    status.transferred_bytes =
        __atomic_load_n(&task.transferred_bytes, __ATOMIC_ACQUIRE);
    status.s = PENDING;
    if (failed + success ==
        __atomic_load_n(&task.slice_count, __ATOMIC_ACQUIRE)) {
        status.s = failed ? FAILED : COMPLETED;
        __atomic_store_n(&task.is_finished, true, __ATOMIC_RELEASE);
    }
    return Status::OK();
}
}  // namespace mooncake
