#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <sys/mman.h>
#include <ubs_mem.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mooncake {
namespace {
std::atomic_bool faulted{false};

int32_t onFault(const char*, ubsmem_fault_type_t) {
    faulted.store(true, std::memory_order_release);
    return 0;
}

std::string requiredSetting(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) throw std::runtime_error(std::string("Set ") + name);
    return value;
}

class SdkContext {
   public:
    SdkContext() {
        // The mock cannot qualify cache visibility or faults on hardware.
        if (requiredSetting("MC_UBSMEM_SDK_QUALIFIED") != "1") {
            throw std::runtime_error("UBS hardware ownership is not qualified");
        }
        ubsmem_options_t options{};
        if (ubsmem_init_attributes(&options) != 0 ||
            ubsmem_initialize(&options) != 0) {
            throw std::runtime_error("Cannot initialize UBS Memory SDK");
        }
        faulted.store(false, std::memory_order_release);
        if (ubsmem_shmem_faults_register(onFault) != 0) {
            ubsmem_finalize();
            throw std::runtime_error("Cannot register UBS fault callback");
        }
    }
    ~SdkContext() { ubsmem_finalize(); }
};

std::shared_ptr<SdkContext> context() {
    // Factories are serialized by the process mapping registry.
    static std::weak_ptr<SdkContext> instance;
    auto result = instance.lock();
    if (!result) {
        result = std::make_shared<SdkContext>();
        instance = result;
    }
    return result;
}

class SdkArena final : public UbsMemoryArena {
   public:
    SdkArena(UbsMemoryDescriptor descriptor, bool owner)
        : UbsMemoryArena(std::move(descriptor), owner), context_(context()) {}

    ~SdkArena() override {
        if (base_) ubsmem_shmem_unmap(base_, size());
    }

    void open() {
        const bool cached = descriptor_.access == "cache";
        // Cache mode needs snoop off and import-noncache needs snoop on; the
        // service rejects the mode the BIOS setting does not support.
        // WR_DELAY_COMP selects the non-relay mode the guide requires for
        // non-cached consistency.
        const uint64_t flags =
            cached ? UBSM_FLAG_CACHE
                   : UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;
        if (descriptor_.backend != "sdk" || descriptor_.host != "ub-domain" ||
            descriptor_.domain != requiredSetting("MC_UBSMEM_DOMAIN") ||
            descriptor_.object_name != "mc-" + descriptor_.generation) {
            throw std::invalid_argument("UBS SDK descriptor mismatch");
        }
        if (owner_) {
            int result;
            if (const char* host = std::getenv("MC_UBSMEM_PROVIDER")) {
                ubs_mem_provider_t provider{};
                if (std::strlen(host) >= sizeof(provider.host_name)) {
                    throw std::invalid_argument("UBS provider name too long");
                }
                std::strcpy(provider.host_name, host);
                provider.socket_id = UINT32_MAX;
                provider.numa_id = UINT32_MAX;
                provider.port_id = UINT32_MAX;
                result = ubsmem_shmem_allocate_with_provider(
                    &provider, descriptor_.object_name.c_str(), size(), 0600,
                    flags);
            } else {
                const char* region = std::getenv("MC_UBSMEM_REGION");
                result =
                    ubsmem_shmem_allocate(region ? region : "default",
                                          descriptor_.object_name.c_str(),
                                          size(), 0600, flags);
            }
            if (result != 0) throw std::runtime_error("UBS allocation failed");
        }
        ubsmem_shmem_info_t info{};
        if (ubsmem_shmem_lookup(descriptor_.object_name.c_str(), &info) != 0 ||
            !info.size || info.size % kAlignment != 0 ||
            (!owner_ && info.size != size())) {
            throw std::runtime_error("UBS object lookup/size mismatch");
        }
        descriptor_.object_size = info.size;
        const int prot = cached ? PROT_NONE : PROT_READ | PROT_WRITE;
        if (ubsmem_shmem_map(nullptr, size(), prot, MAP_SHARED,
                             descriptor_.object_name.c_str(), 0, &base_) != 0) {
            base_ = nullptr;
            throw std::runtime_error("UBS object mapping failed");
        }
    }

   private:
    // Cache mode: granting access invalidates stale lines and revoking it
    // writes back and invalidates this range. Pages without access cannot
    // be cached between copies. No distributed lock is taken.
    bool setOwnership(size_t offset, size_t length, int prot) {
        auto* start = static_cast<char*>(base_) + offset;
        return ubsmem_shmem_set_ownership(descriptor_.object_name.c_str(),
                                          start, length, prot) == 0;
    }

    bool acquire(size_t offset, size_t length, bool write,
                 Deadline deadline) override {
        if (failed_ || faulted.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (descriptor_.access != "cache") return true;
        if (!setOwnership(offset, length,
                          write ? PROT_READ | PROT_WRITE : PROT_READ)) {
            failed_ = true;
            throw std::runtime_error("UBS SDK ownership is uncertain");
        }
        return true;
    }

    bool release(size_t offset, size_t length) override {
        if ((descriptor_.access == "cache" &&
             !setOwnership(offset, length, PROT_NONE)) ||
            faulted.load(std::memory_order_acquire)) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool removeBacking() override {
        if (failed_ || faulted.load(std::memory_order_acquire) ||
            ubsmem_shmem_unmap(base_, size()) != 0) {
            return false;
        }
        base_ = nullptr;
        return ubsmem_shmem_deallocate(descriptor_.object_name.c_str()) == 0;
    }

    std::shared_ptr<SdkContext> context_;
    // Workers copy disjoint ranges concurrently and may all report failure.
    std::atomic_bool failed_{false};
};
}  // namespace

std::shared_ptr<UbsMemoryArena> createUbsMemoryBackend(size_t size) {
    UbsMemoryDescriptor descriptor;
    descriptor.backend = "sdk";
    descriptor.domain = requiredSetting("MC_UBSMEM_DOMAIN");
    descriptor.host = "ub-domain";
    std::ifstream uuid("/proc/sys/kernel/random/uuid");
    if (!(uuid >> descriptor.generation)) {
        throw std::runtime_error("Cannot create a unique UBS object name");
    }
    descriptor.object_name = "mc-" + descriptor.generation;
    descriptor.object_size = size;
    descriptor.access = UbsMemoryArena::accessMode();
    auto arena = std::make_shared<SdkArena>(descriptor, true);
    arena->open();
    return arena;
}

std::shared_ptr<UbsMemoryArena> importUbsMemoryBackend(
    const UbsMemoryDescriptor& descriptor) {
    auto arena = std::make_shared<SdkArena>(descriptor, false);
    arena->open();
    return arena;
}
}  // namespace mooncake
