#include "real_client.h"
#include "client_service.h"
#include "master_client.h"

#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<char> payload() {
    std::vector<char> data(64 * 1024);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (i * 17 + 29) % 256;
    return data;
}

void verify(mooncake::RealClient& client, const std::string& key) {
    auto result = client.get_buffer(key);
    require(result != nullptr, "get_buffer");
    const auto expected = payload();
    require(result->size() == expected.size(), "result size");
    require(std::memcmp(result->ptr(), expected.data(), expected.size()) == 0,
            "payload differs");
}

void exercisePool(mooncake::Client& client, const char* endpoint) {
    mooncake::MasterClient references(mooncake::generate_uuid());
    require(references.Connect(endpoint) == mooncake::ErrorCode::OK,
            "reference client connect");
    constexpr size_t bytes = 64 * 1024;
    std::vector<char> staging(4 * bytes);
    std::fill_n(staging.data(), bytes, 11);
    std::fill_n(staging.data() + bytes, bytes, 22);
    require(client
                .RegisterLocalMemory(staging.data(), staging.size(), "cpu:0",
                                     false, true)
                .has_value(),
            "register boundary staging");
    mooncake::ReplicateConfig config;
    std::vector<mooncake::Slice> value{{staging.data(), bytes}};
    require(client.Put("retired", value, config).has_value(), "put retired");
    auto old = client.Query("retired");
    require(old.has_value(), "query retired");
    auto address = old->replicas.front()
                       .get_memory_descriptor()
                       .buffer_descriptor.buffer_address_;
    require(client.Remove("retired", true).has_value(), "remove retired");
    require(client.Put("replacement", value, config).has_value(),
            "replacement");
    auto replacement = client.Query("replacement");
    require(replacement.has_value() &&
                replacement->replicas.front()
                        .get_memory_descriptor()
                        .buffer_descriptor.buffer_address_ == address,
            "Store must reuse removed allocation");
    require(!client.Get("retired", *old, value), "reject stale read");
    require(client.TransferWriteRange(old->replicas.front(), value, 0) !=
                mooncake::ErrorCode::OK,
            "reject stale write");
    require(client.Get("replacement", value).has_value(), "replacement valid");

    std::vector<std::string> keys{"batch-a", "batch-b"};
    std::vector<std::vector<mooncake::Slice>> writes{
        {{staging.data(), bytes}}, {{staging.data() + bytes, bytes}}};
    config.prefer_alloc_in_same_node = true;
    for (const auto& result : client.BatchPut(keys, writes, config)) {
        require(result.has_value(), "batch put");
    }
    std::unordered_map<std::string, std::vector<mooncake::Slice>> reads{
        {"batch-a", {{staging.data() + 2 * bytes, bytes}}},
        {"batch-b", {{staging.data() + 3 * bytes, bytes}}}};
    for (const auto& result : client.BatchGet(keys, reads)) {
        require(result.has_value(), "batch get");
    }
    require(
        std::memcmp(staging.data(), staging.data() + 2 * bytes, 2 * bytes) == 0,
        "batch payloads");

    auto batch_a = client.Query("batch-a");
    auto batch_b = client.Query("batch-b");
    require(batch_a.has_value() && batch_b.has_value(), "batch descriptors");
    std::fill_n(staging.data() + 2 * bytes, 2 * bytes, 0);
    for (const auto& result :
         client.BatchGet(keys, {*batch_a, *batch_b}, reads, true)) {
        require(result.has_value(), "grouped batch get");
    }
    require(
        std::memcmp(staging.data(), staging.data() + 2 * bytes, 2 * bytes) == 0,
        "grouped batch payloads");
    require(client.Remove("batch-b", true).has_value(), "retire batch key");
    require(client.Put("batch-b", value, config).has_value(),
            "reuse batch key");
    // Pin the first, live descriptor, then reject the second, stale one.
    // The first reference must be released even though no batch was submitted.
    for (const auto& result :
         client.BatchGet(keys, {*batch_a, *batch_b}, reads, true)) {
        require(!result, "grouped stale batch must fail");
    }
    config.prefer_alloc_in_same_node = false;

    char unregistered[64]{};
    std::vector<mooncake::Slice> invalid{{unregistered, sizeof(unregistered)}};
    require(!client.Put("failed-write", invalid, config), "failed write");
    require(!client.Query("failed-write"), "failed write must not publish");
    require(!client.Upsert("replacement", value, config), "reject upsert");
    require(!client.CreateCopyTask("replacement", {"other"}), "reject copy");
    require(!client.CreateMoveTask("replacement", "source", "other"),
            "reject move");
    auto session = client.StartBatchPutForSizes({"session"}, {bytes}, config);
    require(session.size() == 1 && !session.front(), "reject write sessions");

    // More churn than the pool can hold at once must not consume capacity.
    for (int i = 0; i < 100; ++i) {
        require(client.Put("churn", value, config).has_value(), "repeated put");
        require(client.Remove("churn", true).has_value(), "repeated remove");
    }
    size_t stored = 0;
    for (; stored < 80; ++stored) {
        if (!client.Put("fill-" + std::to_string(stored), value, config)) break;
    }
    require(stored > 0 && stored < 80, "bounded pool must exhaust");
    auto held = client.Query("fill-0");
    require(held.has_value(), "query held allocation");
    auto pin = references.BeginUbsMemoryTransfer(
        held->replicas.front().get_memory_descriptor().buffer_descriptor);
    require(pin.has_value(), "pin through master RPC");
    require(client.Remove("fill-0", true).has_value(), "remove from full pool");
    require(!client.Put("while-pinned", value, config),
            "deleted range must wait for active transfer");
    require(references.EndUbsMemoryTransfer(*pin).has_value(), "release pin");
    require(references.EndUbsMemoryTransfer(*pin).has_value(),
            "repeat release");
    require(client.Put("after-full-remove", value, config).has_value(),
            "deleting a key must restore pool capacity");
    require(
        !references.BeginUbsMemoryTransfer(
            held->replicas.front().get_memory_descriptor().buffer_descriptor),
        "stale allocation must not acquire a reference");
    for (size_t i = 1; i < stored; ++i) {
        require(client.Remove("fill-" + std::to_string(i), true).has_value(),
                "remove remaining fill key");
    }
    for (const auto* key :
         {"replacement", "batch-a", "batch-b", "after-full-remove"}) {
        require(client.Remove(key, true).has_value(), "remove remaining key");
    }
    std::vector<char> full_pool(4 * 1024 * 1024, 37);
    require(client
                .RegisterLocalMemory(full_pool.data(), full_pool.size(),
                                     "cpu:0", false, true)
                .has_value(),
            "register full pool payload");
    std::vector<mooncake::Slice> full_value{
        {full_pool.data(), full_pool.size()}};
    require(client.Put("full-pool", full_value, config).has_value(),
            "all deleted ranges must coalesce without leaked references");
    require(client.Remove("full-pool", true).has_value(), "remove full pool");
    require(client.unregisterLocalMemory(full_pool.data()).has_value(),
            "unregister full pool payload");
    require(client.unregisterLocalMemory(staging.data()).has_value(),
            "unregister boundary staging");
}
}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    try {
        require(argc == 4,
                "Usage: ubsmem_store_test provider|producer|consumer master "
                "session");
        std::string role = argv[1];
        const bool tcp = role.starts_with("tcp-");
        if (tcp) role.erase(0, 4);
        const std::string session = argv[3];
        const bool provider = role == "provider";
        setenv("MC_UBSMEM_MOCK_DIR", session.c_str(), 1);
        // A user override must not bypass ownership for self-targeted copies.
        setenv("MC_STORE_MEMCPY", "1", 1);
        if (role == "boundaries" || role == "reject-master") {
            auto arena = mooncake::UbsMemoryArena::create(4 * 1024 * 1024);
            auto result = mooncake::Client::Create(
                "127.0.0.1:0", "P2PHANDSHAKE", "ubsmem", std::nullopt, argv[2]);
            require(result.has_value(), "boundary client");
            auto low = std::move(*result);
            require(!low->MountSegment(arena->base(), arena->size(), "tcp"),
                    "mismatched mount protocol must fail");
            auto mounted = low->MountSegmentAndGetId(arena->base(),
                                                     arena->size(), "ubsmem");
            if (role == "reject-master") {
                require(!mounted && mounted.error() ==
                                        mooncake::ErrorCode::NOT_SUPPORTED,
                        "eviction-enabled master must reject UBS pool");
            } else {
                require(mounted.has_value(), "boundary mount");
                exercisePool(*low, argv[2]);
                require(!low->UnmountSegment(arena->base(), arena->size()),
                        "live unmount must fail");
                require(!low->UnmountSegmentById(*mounted),
                        "live unmount by ID");
                low.reset();
                auto next =
                    mooncake::Client::Create("127.0.0.1:0", "P2PHANDSHAKE",
                                             "ubsmem", std::nullopt, argv[2]);
                require(next.has_value(), "next client");
                require(!(*next)->MountSegment(arena->base(), arena->size(),
                                               "ubsmem"),
                        "a pool generation cannot be mounted twice");
            }
            low.reset();
            require(arena->remove(), "boundary pool cleanup");
            return 0;
        }
        const size_t pool_size =
            tcp ? facebook::cachelib::Slab::kSize : 4 * 1024 * 1024;
        auto client = mooncake::RealClient::create();
        require(client->setup_real("127.0.0.1", "P2PHANDSHAKE",
                                   provider ? pool_size : 0, 4 * 1024 * 1024,
                                   tcp ? "tcp" : "ubsmem", "", argv[2]) == 0,
                "setup_real ubsmem");
        mooncake::ReplicateConfig config;
        config.replica_num = 1;
        const auto data = payload();
        if (provider) {
            require(client->put("owner-local", data, config) == 0, "local put");
            verify(*client, "owner-local");
            std::ofstream(session + "/ready").put('1');
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3600);
            while (!std::filesystem::exists(session + "/stop")) {
                require(std::chrono::steady_clock::now() < deadline,
                        "provider stop timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } else if (role == "producer") {
            require(client->put("shared-payload", data, config) == 0, "put");
        } else if (role == "consumer") {
            verify(*client, "shared-payload");
            require(!client->get_buffer("missing-key"), "missing key");
        } else {
            throw std::runtime_error("Unknown role");
        }
        require(client->tearDownAll() == 0, "teardown");
        std::cout << "UBSMEM_STORE_OK " << role << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
