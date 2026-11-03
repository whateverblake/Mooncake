#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace mooncake {
namespace {
std::mutex registry_mutex;
std::unordered_map<std::string, std::weak_ptr<UbsMemoryArena>> registry;

bool validName(const std::string& name) {
    return !name.empty() && name.size() <= 47 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
}

std::string identity(const UbsMemoryDescriptor& desc) {
    return desc.backend + ":" + desc.domain + ":" + desc.object_name;
}
}  // namespace

bool UbsMemoryDescriptor::valid() const {
    return version == 1 && (backend == "mock" || backend == "sdk") &&
           !domain.empty() && !host.empty() && validName(object_name) &&
           validName(generation) && object_size > 0 &&
           object_size % UbsMemoryArena::kAlignment == 0 &&
           object_size <= std::numeric_limits<size_t>::max() &&
           object_offset < object_size;
}

std::shared_ptr<UbsMemoryArena> UbsMemoryArena::create(size_t size) {
    if (!size || size > std::numeric_limits<size_t>::max() - kAlignment) {
        throw std::invalid_argument("Invalid UBS arena size");
    }
    size = ((size + kAlignment - 1) / kAlignment) * kAlignment;
    std::lock_guard lock(registry_mutex);
    auto arena = createUbsMemoryBackend(size);
    registry[identity(arena->descriptor())] = arena;
    return arena;
}

std::shared_ptr<UbsMemoryArena> UbsMemoryArena::import(
    const UbsMemoryDescriptor& descriptor) {
    if (!descriptor.valid()) {
        throw std::invalid_argument("Invalid UBS object descriptor");
    }
    auto whole = descriptor;
    whole.object_offset = 0;
    std::lock_guard lock(registry_mutex);
    auto& entry = registry[identity(whole)];
    if (auto arena = entry.lock()) {
        if (arena->descriptor() != whole || !arena->base()) {
            throw std::invalid_argument("Stale UBS object descriptor");
        }
        return arena;
    }
    auto arena = importUbsMemoryBackend(whole);
    entry = arena;
    return arena;
}

std::shared_ptr<UbsMemoryArena> UbsMemoryArena::findOwned(void* addr,
                                                          size_t size) {
    const auto pointer = reinterpret_cast<uintptr_t>(addr);
    std::lock_guard lock(registry_mutex);
    for (const auto& [key, entry] : registry) {
        auto arena = entry.lock();
        if (!arena || !arena->owner_ || !arena->base()) continue;
        const auto start = reinterpret_cast<uintptr_t>(arena->base());
        if (pointer >= start && pointer - start < arena->size() && size &&
            size <= arena->size() - (pointer - start)) {
            return arena;
        }
    }
    return nullptr;
}

bool UbsMemoryArena::overlaps(const void* addr, size_t size) {
    if (!addr || !size) return false;
    const auto pointer = reinterpret_cast<uintptr_t>(addr);
    if (size > std::numeric_limits<uintptr_t>::max() - pointer) return true;
    std::lock_guard lock(registry_mutex);
    for (const auto& [key, entry] : registry) {
        auto arena = entry.lock();
        if (!arena || !arena->base()) continue;
        const auto start = reinterpret_cast<uintptr_t>(arena->base());
        if (pointer < start + arena->size() && start < pointer + size) {
            return true;
        }
    }
    return false;
}

bool UbsMemoryArena::copy(size_t offset, void* buffer, size_t length,
                          bool write, Deadline deadline) {
    if (!buffer || !length || offset >= size() || length > size() - offset ||
        overlaps(buffer, length)) {
        return false;
    }
    std::unique_lock lock(mutex_, std::defer_lock);
    if (!lock.try_lock_until(deadline) || failed_ || !base_ ||
        std::chrono::steady_clock::now() >= deadline) {
        return false;
    }
    try {
        if (!acquire(write, deadline)) return false;
        auto* target = static_cast<char*>(base_) + offset;
        if (write) {
            std::memcpy(target, buffer, length);
        } else {
            std::memcpy(buffer, target, length);
        }
        if (!release()) {
            throw std::runtime_error("UBS ownership release is uncertain");
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
    return true;
}

bool UbsMemoryArena::remove() {
    std::lock_guard registry_lock(registry_mutex);
    std::lock_guard lock(mutex_);
    if (!owner_ || !base_ || failed_ || weak_from_this().use_count() != 1) {
        return false;
    }
    return removeBacking();
}
}  // namespace mooncake
