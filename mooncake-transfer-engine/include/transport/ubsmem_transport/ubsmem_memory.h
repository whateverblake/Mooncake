#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "transport/ubsmem_transport/ubsmem_descriptor.h"

namespace mooncake {

// A mapping is private to this process. Its pointer is only an address token
// outside copy(); the backing pages have no read or write permission.
class UbsMemoryArena : public std::enable_shared_from_this<UbsMemoryArena> {
   public:
    using Deadline = std::chrono::steady_clock::time_point;
    static constexpr size_t kAlignment = 4 * 1024 * 1024;

    virtual ~UbsMemoryArena() = default;
    static std::shared_ptr<UbsMemoryArena> create(size_t size);
    static std::shared_ptr<UbsMemoryArena> import(
        const UbsMemoryDescriptor& descriptor);
    static std::shared_ptr<UbsMemoryArena> findOwned(void* addr, size_t size);
    static bool overlaps(const void* addr, size_t size);

    void* base() const { return base_; }
    size_t size() const { return descriptor_.object_size; }
    const UbsMemoryDescriptor& descriptor() const { return descriptor_; }
    bool claimStorePool() { return owner_ && !store_claimed_.exchange(true); }

    bool copy(size_t offset, void* buffer, size_t length, bool write,
              Deadline deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(30));

    // Explicit cleanup after all clients have drained. Destruction only
    // unmaps locally: uncertain or abandoned sessions retain their files.
    bool remove();

   protected:
    UbsMemoryArena(UbsMemoryDescriptor descriptor, bool owner)
        : descriptor_(std::move(descriptor)), owner_(owner) {}
    virtual bool acquire(bool write, Deadline deadline) = 0;
    virtual bool release() = 0;
    virtual bool removeBacking() = 0;

    UbsMemoryDescriptor descriptor_;
    void* base_ = nullptr;
    bool owner_;

   private:
    std::timed_mutex mutex_;
    bool failed_ = false;
    std::atomic_bool store_claimed_{false};
};

// Exactly one backend is selected at build time.
std::shared_ptr<UbsMemoryArena> createUbsMemoryBackend(size_t size);
std::shared_ptr<UbsMemoryArena> importUbsMemoryBackend(
    const UbsMemoryDescriptor& descriptor);

}  // namespace mooncake
