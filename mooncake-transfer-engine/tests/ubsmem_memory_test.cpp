#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

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
    bool acquire(bool, Deadline) override {
        return mprotect(base_, size(), PROT_READ | PROT_WRITE) == 0;
    }
    bool release() override {
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
        require(arena->remove(), "remove quiescent arena");
        require(!arena->copy(0, &value, 1, true), "removed arena rejected");
        arena.reset();
        std::filesystem::remove_all(directory);
        std::cout << "UBSMEM_MEMORY_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
