#include "client_service.h"
#include "master_client.h"
#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <sys/mman.h>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace mooncake {
namespace {
// Test-only backend: the copy finishes, but ownership release fails.
class FailingArena final : public UbsMemoryArena {
   public:
    explicit FailingArena(size_t bytes)
        : UbsMemoryArena(
              {1, "mock", "test", "test", "fault", "1", bytes, 0, "cache"},
              true) {
        base_ =
            mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) throw std::runtime_error("fault arena mmap");
    }
    ~FailingArena() override { munmap(base_, size()); }

   private:
    bool acquire(size_t, size_t, bool, Deadline) override {
        return mprotect(base_, size(), PROT_READ | PROT_WRITE) == 0;
    }
    bool release(size_t, size_t) override {
        mprotect(base_, size(), PROT_NONE);
        return false;
    }
    bool removeBacking() override { return false; }
};
}  // namespace

// These symbols replace the mock backend only in this test executable.
std::shared_ptr<UbsMemoryArena> createUbsMemoryBackend(size_t size) {
    return std::make_shared<FailingArena>(size);
}
std::shared_ptr<UbsMemoryArena> importUbsMemoryBackend(
    const UbsMemoryDescriptor&) {
    throw std::runtime_error("Fault test should use its local owner mapping");
}
}  // namespace mooncake

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    try {
        require(argc == 4, "fault test requires role, master, session");
        auto arena = mooncake::UbsMemoryArena::create(4 * 1024 * 1024);
        auto result = mooncake::Client::Create("127.0.0.1:0", "P2PHANDSHAKE",
                                               "ubsmem", std::nullopt, argv[2]);
        require(result.has_value(), "fault client");
        auto client = *result;
        require(client->MountSegment(arena->base(), arena->size(), "ubsmem")
                    .has_value(),
                "fault pool mount");
        mooncake::MasterClient master(mooncake::generate_uuid());
        require(master.Connect(argv[2]) == mooncake::ErrorCode::OK,
                "fault master connection");
        std::vector<char> data(arena->size(), 42);
        require(client
                    ->RegisterLocalMemory(data.data(), data.size(), "cpu:0",
                                          false, true)
                    .has_value(),
                "fault staging registration");
        mooncake::ReplicateConfig config;
        auto replicas = master.PutStart("uncertain", {data.size()}, config);
        require(replicas.has_value() && replicas->size() == 1,
                "reserve entire pool");
        std::vector<mooncake::Slice> slices{{data.data(), data.size()}};
        require(client->TransferWriteRange(replicas->front(), slices, 0) !=
                    mooncake::ErrorCode::OK,
                "ownership failure must fail transfer");
        require(master.PutRevoke("uncertain", mooncake::ReplicaType::MEMORY)
                    .has_value(),
                "retire uncertain allocation");
        require(!master.PutStart("must-not-reuse", {data.size()}, config),
                "uncertain ownership must retain the master reference");
        require(client->unregisterLocalMemory(data.data()).has_value(),
                "copy stopped using staging memory");
        std::cout << "UBSMEM_OWNERSHIP_FAILURE_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
