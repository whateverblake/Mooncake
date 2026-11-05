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
    char access[16];
};

bool sameText(const char* field, size_t capacity, const std::string& value) {
    return strnlen(field, capacity) == value.size() &&
           std::memcmp(field, value.data(), value.size()) == 0;
}

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
            std::memcpy(header.access, descriptor_.access.c_str(),
                        descriptor_.access.size());
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
            !sameText(header.generation, sizeof(header.generation),
                      descriptor_.generation) ||
            !sameText(header.access, sizeof(header.access),
                      descriptor_.access)) {
            throw std::invalid_argument("Stale UBS mock backing object");
        }
        // A non-cached import needs no per-copy permission changes.
        const int prot = cached() ? PROT_NONE : PROT_READ | PROT_WRITE;
        void* mapping =
            mmap(nullptr, size(), prot, MAP_SHARED, fd_, kHeaderBytes);
        if (mapping == MAP_FAILED) {
            throw std::runtime_error("Cannot map UBS mock object");
        }
        base_ = mapping;
    }

   private:
    bool cached() const { return descriptor_.access == "cache"; }

    // Mock-only check of the master's exclusivity. A byte-range lock on the
    // copied pages never conflicts in correct use, so it does not serialize
    // the pool. Tests hold a range to simulate a stalled peer.
    bool rangeLock(size_t offset, size_t length, short type) {
        struct flock range{};
        range.l_type = type;
        range.l_whence = SEEK_SET;
        range.l_start = kHeaderBytes + static_cast<off_t>(offset);
        range.l_len = static_cast<off_t>(length);
        return fcntl(fd_, F_OFD_SETLK, &range) == 0;
    }

    bool acquire(size_t offset, size_t length, bool write,
                 Deadline deadline) override {
        while (!rangeLock(offset, length, write ? F_WRLCK : F_RDLCK)) {
            if ((errno != EAGAIN && errno != EACCES && errno != EINTR) ||
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto* start = static_cast<char*>(base_) + offset;
        if (cached() && mprotect(start, length,
                                 write ? PROT_READ | PROT_WRITE : PROT_READ) !=
                            0) {
            if (!rangeLock(offset, length, F_UNLCK)) {
                throw std::runtime_error("UBS rollback unlock is uncertain");
            }
            return false;
        }
        return true;
    }

    bool release(size_t offset, size_t length) override {
        // Do not let another copy start if revoking our access fails.
        auto* start = static_cast<char*>(base_) + offset;
        if (cached() && mprotect(start, length, PROT_NONE) != 0) return false;
        return rangeLock(offset, length, F_UNLCK);
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
    descriptor.access = UbsMemoryArena::accessMode();
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
