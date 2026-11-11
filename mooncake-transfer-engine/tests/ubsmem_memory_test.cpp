#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using mooncake::UbsMemoryArena;
using mooncake::UbsMemoryDescriptor;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

class FailingReleaseArena final : public UbsMemoryArena {
   public:
    FailingReleaseArena() : UbsMemoryArena(UbsMemoryDescriptor{}, true) {
        descriptor_.object_size = kAlignment;
        base_ = mmap(nullptr, size(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
        require(base_ != MAP_FAILED, "fault test mapping");
    }
    ~FailingReleaseArena() override { munmap(base_, size()); }

   private:
    bool acquire(size_t, size_t, bool, Deadline) override {
        return mprotect(base_, size(), PROT_READ | PROT_WRITE) == 0;
    }
    bool release(size_t, size_t) override {
        require(mprotect(base_, size(), PROT_NONE) == 0, "fault test revoke");
        return false;
    }
    bool removeBacking() override {
        throw std::runtime_error("Quarantined arena must not be removed");
    }
};

int main(int argc, char** argv) {
    try {
        if (argc == 8) {
            UbsMemoryDescriptor desc;
            desc.backend = "mock";
            desc.object_name = argv[1];
            desc.generation = argv[2];
            desc.domain = argv[3];
            desc.host = argv[4];
            desc.object_size = std::stoull(argv[5]);
            desc.access = "cache";
            void* parent_base = reinterpret_cast<void*>(std::stoull(argv[7]));
            void* reserved =
                mmap(parent_base, desc.object_size, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            require(reserved != MAP_FAILED || errno == EEXIST,
                    "reserve parent's virtual address");
            auto imported = UbsMemoryArena::import(desc);
            require(imported->base() != parent_base,
                    "different process mapping addresses");
            unsigned char value = 0;
            require(imported->copy(123, &value, 1, false), "child read");
            require(value == 42, "cross-process visibility");
            value = 99;
            require(imported->copy(124, &value, 1, true), "child write");
            const std::string control = argv[6];
            std::ofstream(control + ".ready").put('1');
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!std::filesystem::exists(control + ".stop")) {
                require(std::chrono::steady_clock::now() < deadline,
                        "child stop timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (reserved != MAP_FAILED) munmap(reserved, desc.object_size);
            return 0;
        }

        char directory[] = "/tmp/mooncake-ubsmem-test-XXXXXX";
        require(mkdtemp(directory) != nullptr, "session directory");
        setenv("MC_UBSMEM_MOCK_DIR", directory, 1);
        auto faulty = std::make_shared<FailingReleaseArena>();
        unsigned char byte = 7;
        bool uncertain = false;
        try {
            faulty->copy(0, &byte, 1, true);
        } catch (const std::runtime_error&) {
            uncertain = true;
        }
        require(uncertain, "release failure must report uncertain ownership");
        require(!faulty->copy(0, &byte, 1, true),
                "failed arena stays quarantined");
        require(!faulty->remove(), "uncertain arena cannot be removed");
        faulty.reset();
        auto arena = UbsMemoryArena::create(4 * 1024 * 1024);
        require(arena->size() == 4 * 1024 * 1024, "arena size");
        unsigned char value = 42;
        require(arena->copy(123, &value, 1, true), "write");
        const auto desc = arena->descriptor();
        auto same = UbsMemoryArena::import(desc);
        require(same == arena, "one mapping per process");
        require(!arena->remove(), "cannot remove referenced arena");
        same.reset();

        const pid_t child = fork();
        require(child >= 0, "fork");
        if (child == 0) {
            const auto size = std::to_string(desc.object_size);
            const auto control = std::string(directory) + "/child";
            const auto parent_base =
                std::to_string(reinterpret_cast<uintptr_t>(arena->base()));
            execl(argv[0], argv[0], desc.object_name.c_str(),
                  desc.generation.c_str(), desc.domain.c_str(),
                  desc.host.c_str(), size.c_str(), control.c_str(),
                  parent_base.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (
            !std::filesystem::exists(std::string(directory) + "/child.ready")) {
            require(std::chrono::steady_clock::now() < deadline,
                    "child ready timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(!arena->remove(), "cannot remove remotely mapped arena");
        std::ofstream(std::string(directory) + "/child.stop").put('1');
        require(waitpid(child, &status, 0) == child, "wait child");
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "child transfer");
        require(arena->copy(124, &value, 1, false) && value == 99,
                "parent sees child write");

        require(!arena->copy(arena->size(), &value, 1, true), "past end");
        require(
            !arena->copy(std::numeric_limits<size_t>::max(), &value, 2, true),
            "overflow");
        require(!arena->copy(0, arena->base(), 1, false), "arena alias");
        auto bad = desc;
        bad.generation = "wrong-generation";
        bool rejected = false;
        try {
            UbsMemoryArena::import(bad);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "reject stale identity");

        const pid_t access = fork();
        require(access >= 0, "fork protection check");
        if (access == 0) {
            auto* ptr = static_cast<volatile unsigned char*>(arena->base());
            *ptr = 1;
            _exit(0);
        }
        require(waitpid(access, &status, 0) == access, "wait protection");
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
                "mapping inaccessible outside guard");

        // Hold the mock's check on the first page only, as a peer stalled
        // mid-copy would. Other ranges of the same pool stay usable.
        const auto path = std::string(directory) + "/" + desc.object_name;
        const int peer = open(path.c_str(), O_RDWR | O_CLOEXEC);
        struct stat file{};
        require(peer >= 0 && fstat(peer, &file) == 0, "open peer");
        struct flock range{};
        range.l_type = F_WRLCK;
        range.l_whence = SEEK_SET;
        range.l_start = file.st_size - static_cast<off_t>(arena->size());
        const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        range.l_len = static_cast<off_t>(page);
        require(fcntl(peer, F_OFD_SETLK, &range) == 0, "stall first page");
        const auto soon = [] {
            return std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(50);
        };
        require(!arena->copy(0, &value, 1, true, soon()),
                "stalled range waits until its deadline");
        require(arena->copy(2 * page, &value, 1, true, soon()),
                "other ranges of the pool are not locked");
        require(arena->copy(2 * page, &value, 1, false, soon()),
                "other ranges remain readable");
        close(peer);
        require(arena->copy(0, &value, 1, true, soon()), "range released");

        // Overlapping copies in one process wait for each other: releasing
        // one copy's pages must not revoke them under another copy.
        std::atomic_bool overlapping_ok{true};
        std::vector<std::thread> copiers;
        for (int t = 0; t < 4; ++t) {
            copiers.emplace_back([&, t] {
                std::vector<unsigned char> data(3 * page,
                                                static_cast<unsigned char>(t));
                for (int i = 0; i < 200 && overlapping_ok; ++i) {
                    if (!arena->copy(page / 2 + t, data.data(), data.size(),
                                     i % 2 == 0)) {
                        overlapping_ok = false;
                    }
                }
            });
        }
        for (auto& copier : copiers) copier.join();
        require(overlapping_ok, "overlapping concurrent copies");
        require(arena->remove(), "remove quiescent arena");
        require(!arena->copy(0, &value, 1, true), "removed arena rejected");
        arena.reset();

        setenv("MC_UBSMEM_ACCESS_MODE", "bogus", 1);
        bool bad_mode = false;
        try {
            UbsMemoryArena::create(4 * 1024 * 1024);
        } catch (const std::invalid_argument&) {
            bad_mode = true;
        }
        require(bad_mode, "reject unknown access mode");
        setenv("MC_UBSMEM_ACCESS_MODE", "import-noncache", 1);
        auto uncached = UbsMemoryArena::create(4 * 1024 * 1024);
        unsetenv("MC_UBSMEM_ACCESS_MODE");
        require(uncached->descriptor().access == "import-noncache",
                "descriptor records access mode");
        value = 55;
        require(uncached->copy(4096, &value, 1, true), "non-cached write");
        const pid_t direct = fork();
        require(direct >= 0, "fork non-cached check");
        if (direct == 0) {
            auto* ptr = static_cast<volatile unsigned char*>(uncached->base());
            _exit(ptr[4096] == 55 ? 0 : 1);
        }
        require(waitpid(direct, &status, 0) == direct, "wait non-cached");
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "non-cached mapping needs no per-copy access change");
        auto mismatched = uncached->descriptor();
        mismatched.access = "cache";
        uncached.reset();
        bool rejected_mode = false;
        try {
            UbsMemoryArena::import(mismatched);
        } catch (const std::exception&) {
            rejected_mode = true;
        }
        require(rejected_mode, "importer must follow the creator's mode");
        std::filesystem::remove_all(directory);
        std::cout << "UBSMEM_MEMORY_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
