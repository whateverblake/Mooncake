// A deliberately small Store -> Transfer Engine -> TCP learning exercise.
#include "client_service.h"
#include "real_client.h"

#include <glog/logging.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using mooncake::Client;
using mooncake::ErrorCode;
using mooncake::Slice;

constexpr size_t kSegmentBytes = 64 * 1024 * 1024;
constexpr size_t kValueBytes = 4096;
constexpr const char* kOwner = "127.0.0.1:12345";
constexpr const char* kClient = "127.0.0.1:12346";
volatile std::sig_atomic_t stop_requested = 0;

void onSignal(int) { stop_requested = 1; }

template <typename T>
void require(const tl::expected<T, ErrorCode>& result, const char* operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 mooncake::toString(result.error()));
    }
}

std::shared_ptr<Client> connect(const char* endpoint) {
    auto result = Client::Create(endpoint, "P2PHANDSHAKE", "tcp",
                                 std::nullopt, "127.0.0.1:50051");
    if (!result) {
        throw std::runtime_error("Client::Create failed; check master.log");
    }
    return *result;
}

// Allocations outlive their Client, including on exceptions. Destruction of
// Client shuts down its engine before these buffers are freed.
using Buffer = std::unique_ptr<unsigned char, decltype(&std::free)>;
Buffer allocate(size_t size) {
    void* memory = nullptr;
    if (posix_memalign(&memory, 4 * 1024 * 1024, size) != 0) {
        throw std::bad_alloc();
    }
    std::memset(memory, 0, size);
    return Buffer(static_cast<unsigned char*>(memory), &std::free);
}

int owner() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    auto memory = allocate(kSegmentBytes);
    auto client = connect(kOwner);
    require(client->MountSegment(memory.get(), kSegmentBytes, "tcp"),
            "MountSegment");
    std::cout << "OWNER_READY endpoint=" << client->GetTransportEndpoint()
              << " bytes=" << kSegmentBytes
              << " base=" << static_cast<void*>(memory.get()) << std::endl;
    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(client->UnmountSegment(memory.get(), kSegmentBytes),
            "UnmountSegment");
    return 0;
}

// Convenience API: RealClient manages transfer buffers and memory registration.
int highlevel() {
    auto check = [](int result, const char* operation) {
        if (result != 0) {
            throw std::runtime_error(std::string(operation) + " failed: " +
                                     std::to_string(result));
        }
    };

    auto client = mooncake::RealClient::create();
    // Break here to follow client creation and Transfer Engine initialization.
    check(client->setup_real("127.0.0.1", "P2PHANDSHAKE",
                             0,                 // No storage contribution
                             16 * 1024 * 1024,  // Managed local transfer buffer
                             "tcp", "", "127.0.0.1:50051"),
          "setup_real");

    const std::string key =
        "debug/highlevel/" + std::to_string(getpid()) + "/" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string value = "Hello, Mooncake!";
    mooncake::ReplicateConfig config;
    config.replica_num = 1;

    // Break here, then step into RealClient::put and Client::Put.
    check(client->put(key, std::span<const char>(value.data(), value.size()),
                      config),
          "put");

    {
        // Break here, then step into RealClient::get_buffer and Client::Get.
        auto buffer = client->get_buffer(key);
        if (!buffer) throw std::runtime_error("get_buffer failed");
        std::string result(static_cast<const char*>(buffer->ptr()),
                           buffer->size());
        std::cout << "Received: " << result << std::endl;
        if (result != value) {
            throw std::runtime_error("Get returned different bytes");
        }
    }  // Release the result buffer before shutting down the client.
    std::cout << "HIGHLEVEL_ROUNDTRIP_OK" << std::endl;

    check(client->remove(key, true), "remove");
    // check(client->tearDownAll(), "shutdown");
    std::cout << "CLEANUP_OK" << std::endl;
    return 0;
}

int roundtrip() {
    auto source = allocate(kValueBytes);
    auto destination = allocate(kValueBytes);
    // No MountSegment here: this client never offers storage to the master.
    auto client = connect(kClient);
    require(client->RegisterLocalMemory(source.get(), kValueBytes, "cpu:0"),
            "RegisterLocalMemory(source)");
    require(client->RegisterLocalMemory(destination.get(), kValueBytes, "cpu:0"),
            "RegisterLocalMemory(destination)");

    for (size_t i = 0; i < kValueBytes; ++i) {
        source.get()[i] = static_cast<unsigned char>((i * 17 + 29) % 256);
    }
    // Recognizable prefix for the packet capture; remaining bytes cover 0..255.
    constexpr char marker[] = "MOONCAKE_TCP_DEBUG_PAYLOAD";
    std::memcpy(source.get(), marker, sizeof(marker) - 1);
    std::memset(destination.get(), 0xa5, kValueBytes);

    const std::string key =
        "debug/" + std::to_string(getpid()) + "/" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::vector<Slice> write_slices{{source.get(), kValueBytes}};
    std::vector<Slice> read_slices{{destination.get(), kValueBytes}};
    mooncake::ReplicateConfig config;
    config.replica_num = 1;
    config.preferred_segments = {kOwner};

    // Start here in CLion. Step into Put, TransferWrite, then the TCP worker.
    std::cout << "PUT key=" << key << " bytes=" << kValueBytes << std::endl;
    require(client->Put(key, write_slices, config), "Put");
    auto query = client->Query(key);
    require(query, "Query");
    if (query->replicas.size() != 1 ||
        !query->replicas.front().is_memory_replica()) {
        throw std::runtime_error("Expected exactly one memory replica");
    }
    const auto& replica =
        query->replicas.front().get_memory_descriptor().buffer_descriptor;
    std::cout << "REMOTE_REPLICA endpoint=" << replica.transport_endpoint_
              << " address=" << reinterpret_cast<void*>(replica.buffer_address_)
              << " bytes=" << replica.size_ << std::endl;
    if (replica.transport_endpoint_.empty() ||
        replica.transport_endpoint_ == client->GetTransportEndpoint() ||
        replica.protocol_ != "tcp") {
        throw std::runtime_error("Replica must use TCP in a separate process");
    }

    // Step into Get to observe a fresh lookup followed by a TCP READ.
    require(client->Get(key, read_slices), "Get");
    if (!std::equal(source.get(), source.get() + kValueBytes, destination.get())) {
        throw std::runtime_error("Get returned different bytes");
    }
    std::cout << "ROUNDTRIP_OK bytes=" << kValueBytes << std::endl;

    auto missing = client->Get(key + "/missing", read_slices);
    if (missing || missing.error() != ErrorCode::OBJECT_NOT_FOUND) {
        throw std::runtime_error("Missing-key Get must return OBJECT_NOT_FOUND");
    }
    std::cout << "MISSING_KEY_OK" << std::endl;

    require(client->Remove(key, true), "Remove");
    auto removed = client->Query(key);
    if (removed || removed.error() != ErrorCode::OBJECT_NOT_FOUND) {
        throw std::runtime_error("Removed key is still visible");
    }
    require(client->unregisterLocalMemory(source.get()), "Unregister(source)");
    require(client->unregisterLocalMemory(destination.get()), "Unregister(destination)");
    std::cout << "CLEANUP_OK" << std::endl;
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    // Preserve explicit timeout overrides, but always force the TCP data path.
    setenv("MC_STORE_MEMCPY", "0", 1);
    setenv("MC_TCP_STATUS_TIMEOUT_SEC", "3600", 0);
    setenv("MC_TCP_PROGRESS_TIMEOUT_SEC", "3600", 0);
    try {
        if (argc == 2 && std::string(argv[1]) == "owner") return owner();
        if (argc == 2 && std::string(argv[1]) == "roundtrip") return roundtrip();
        if (argc == 2 && std::string(argv[1]) == "highlevel") return highlevel();
        std::cerr << "Usage: " << argv[0] << " owner|roundtrip|highlevel\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
