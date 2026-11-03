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
        // The mock cannot qualify lock expiry or cache visibility on hardware.
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
                    UBSM_FLAG_WITH_LOCK);
            } else {
                const char* region = std::getenv("MC_UBSMEM_REGION");
                result =
                    ubsmem_shmem_allocate(region ? region : "default",
                                          descriptor_.object_name.c_str(),
                                          size(), 0600, UBSM_FLAG_WITH_LOCK);
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
        if (ubsmem_shmem_map(nullptr, size(), PROT_NONE, MAP_SHARED,
                             descriptor_.object_name.c_str(), 0, &base_) != 0) {
            base_ = nullptr;
            throw std::runtime_error("UBS object mapping failed");
        }
    }

   private:
    bool acquire(bool write, Deadline deadline) override {
        if (failed_ || faulted.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const auto* name = descriptor_.object_name.c_str();
        const int result = write ? ubsmem_shmem_write_lock(name)
                                 : ubsmem_shmem_read_lock(name);
        if (result != 0) {
            failed_ = true;
            throw std::runtime_error("UBS SDK lock ownership is uncertain");
        }
        if (faulted.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
            if (!release()) {
                throw std::runtime_error(
                    "UBS SDK rollback unlock is uncertain");
            }
            return false;
        }
        return true;
    }

    bool release() override {
        if (ubsmem_shmem_unlock(descriptor_.object_name.c_str()) != 0 ||
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
    bool failed_ = false;
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
