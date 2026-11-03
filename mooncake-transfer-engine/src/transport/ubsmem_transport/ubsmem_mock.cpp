#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace mooncake {
namespace {
constexpr off_t kHeaderBytes = 64 * 1024;
constexpr uint64_t kMagic = 0x5542534d454d3031;
struct Header {
    uint64_t magic;
    uint64_t size;
    char generation[48];
};

std::string readIdentity(const char* path) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value))
        throw std::runtime_error("Cannot read host identity");
    return value;
}

std::string directory() {
    const char* path = std::getenv("MC_UBSMEM_MOCK_DIR");
    if (!path || !*path) {
        throw std::runtime_error(
            "Set MC_UBSMEM_MOCK_DIR to a session directory");
    }
    return std::filesystem::canonical(path).string();
}

class MockArena final : public UbsMemoryArena {
   public:
    MockArena(UbsMemoryDescriptor descriptor, bool owner)
        : UbsMemoryArena(std::move(descriptor), owner) {}

    ~MockArena() override {
        if (base_) munmap(base_, size());
        if (fd_ >= 0) close(fd_);
        if (lease_fd_ >= 0) close(lease_fd_);
    }

    void open() {
        path_ = directory() + "/" + descriptor_.object_name;
        if (descriptor_.domain != directory() ||
            descriptor_.host !=
                readIdentity("/proc/sys/kernel/random/boot_id") ||
            descriptor_.backend != "mock" ||
            size() > static_cast<uint64_t>(std::numeric_limits<off_t>::max() -
                                           kHeaderBytes)) {
            throw std::invalid_argument("UBS mock domain/host/size mismatch");
        }
        const int flags =
            O_RDWR | O_CLOEXEC | O_NOFOLLOW | (owner_ ? O_CREAT | O_EXCL : 0);
        lease_fd_ = ::open((path_ + ".lease").c_str(), flags, 0600);
        if (lease_fd_ < 0 || flock(lease_fd_, LOCK_SH | LOCK_NB) != 0) {
            throw std::runtime_error("Cannot pin UBS mock object");
        }
        fd_ = ::open(path_.c_str(), flags, 0600);
        if (fd_ < 0) throw std::runtime_error("Cannot open UBS mock object");
        Header header{};
        if (owner_) {
            header.magic = kMagic;
            header.size = size();
            std::memcpy(header.generation, descriptor_.generation.c_str(),
                        descriptor_.generation.size());
            if (ftruncate(fd_, size() + kHeaderBytes) != 0 ||
                pwrite(fd_, &header, sizeof(header), 0) != sizeof(header)) {
                throw std::runtime_error("Cannot create UBS mock object");
            }
        }
        struct stat statbuf{};
        if (fstat(fd_, &statbuf) != 0 || !S_ISREG(statbuf.st_mode) ||
            statbuf.st_size != static_cast<off_t>(size()) + kHeaderBytes ||
            pread(fd_, &header, sizeof(header), 0) != sizeof(header) ||
            header.magic != kMagic || header.size != size() ||
            strnlen(header.generation, sizeof(header.generation)) !=
                descriptor_.generation.size() ||
            std::memcmp(header.generation, descriptor_.generation.data(),
                        descriptor_.generation.size()) != 0) {
            throw std::invalid_argument("Stale UBS mock backing object");
        }
        void* mapping =
            mmap(nullptr, size(), PROT_NONE, MAP_SHARED, fd_, kHeaderBytes);
        if (mapping == MAP_FAILED) {
            throw std::runtime_error("Cannot map UBS mock object");
        }
        base_ = mapping;
    }

   private:
    bool acquire(bool write, Deadline deadline) override {
        const int operation = (write ? LOCK_EX : LOCK_SH) | LOCK_NB;
        while (flock(fd_, operation) != 0) {
            if ((errno != EWOULDBLOCK && errno != EINTR) ||
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (mprotect(base_, size(),
                     write ? PROT_READ | PROT_WRITE : PROT_READ) != 0) {
            if (flock(fd_, LOCK_UN) != 0) {
                throw std::runtime_error("UBS rollback unlock is uncertain");
            }
            return false;
        }
        return true;
    }

    bool release() override {
        // Do not grant another process access if revoking ours fails.
        if (mprotect(base_, size(), PROT_NONE) != 0) return false;
        return flock(fd_, LOCK_UN) == 0;
    }

    bool removeBacking() override {
        if (flock(lease_fd_, LOCK_EX | LOCK_NB) != 0) {
            flock(lease_fd_, LOCK_SH | LOCK_NB);
            return false;
        }
        if (munmap(base_, size()) != 0) return false;
        base_ = nullptr;
        const bool removed = unlink(path_.c_str()) == 0;
        if (removed) unlink((path_ + ".lease").c_str());
        return removed;
    }

    int fd_ = -1;
    int lease_fd_ = -1;
    std::string path_;
};
}  // namespace

std::shared_ptr<UbsMemoryArena> createUbsMemoryBackend(size_t size) {
    UbsMemoryDescriptor descriptor;
    descriptor.backend = "mock";
    descriptor.domain = directory();
    descriptor.host = readIdentity("/proc/sys/kernel/random/boot_id");
    descriptor.generation = readIdentity("/proc/sys/kernel/random/uuid");
    descriptor.object_name = "mc-" + descriptor.generation;
    descriptor.object_size = size;
    auto arena = std::make_shared<MockArena>(descriptor, true);
    arena->open();
    return arena;
}

std::shared_ptr<UbsMemoryArena> importUbsMemoryBackend(
    const UbsMemoryDescriptor& descriptor) {
    auto arena = std::make_shared<MockArena>(descriptor, false);
    arena->open();
    return arena;
}
}  // namespace mooncake
