#include "transfer_engine.h"
#include "transport/ubsmem_transport/ubsmem_memory.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

mooncake::TransferStatus transfer(mooncake::TransferEngine& engine,
                                  mooncake::TransferRequest request,
                                  bool wait_event = false) {
    auto batch = engine.allocateBatchID(1);
    std::vector<mooncake::TransferRequest> requests{request};
    require(engine.submitTransfer(batch, requests).ok(), "submit");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    mooncake::TransferStatus status{};
#ifdef USE_EVENT_DRIVEN_COMPLETION
    if (wait_event) {
        auto& desc = mooncake::Transport::toBatchDesc(batch);
        std::unique_lock lock(desc.completion_mutex);
        require(desc.completion_cv.wait_until(
                    lock, deadline,
                    [&desc] {
                        return desc.is_finished.load(std::memory_order_acquire);
                    }),
                "event completion notification");
        status.s = desc.has_failure.load() ? mooncake::Transport::FAILED
                                           : mooncake::Transport::COMPLETED;
        status.transferred_bytes = __atomic_load_n(
            &desc.task_list.front().transferred_bytes, __ATOMIC_ACQUIRE);
        lock.unlock();
        require(engine.freeBatchID(batch).ok(),
                "free after event without poll");
        return status;
    }
#endif
    do {
        require(engine.getTransferStatus(batch, 0, status).ok(), "poll");
        if (status.s == mooncake::Transport::COMPLETED ||
            status.s == mooncake::Transport::FAILED) {
            mooncake::TransferStatus aggregate{};
            require(engine.getBatchTransferStatus(batch, aggregate).ok() &&
                        aggregate.s == status.s &&
                        aggregate.transferred_bytes == status.transferred_bytes,
                    "aggregate status preserves failure and bytes");
            require(engine.freeBatchID(batch).ok(), "free completed batch");
            return status;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("transfer timed out");
}
void tcpSmoke() {
    std::array<unsigned char, 64> target{};
    std::array<unsigned char, 64> buffer{};
    mooncake::TransferEngine owner, client;
    require(owner.init("P2PHANDSHAKE", "127.0.0.1:0") == 0, "TCP owner init");
    require(client.init("P2PHANDSHAKE", "127.0.0.1:0") == 0, "TCP client init");
    require(owner.installTransport("tcp", nullptr), "TCP owner transport");
    require(client.installTransport("tcp", nullptr), "TCP client transport");
    require(owner.registerLocalMemory(target.data(), target.size(), "cpu:0",
                                      true) == 0,
            "TCP target registration");
    require(client.registerLocalMemory(buffer.data(), buffer.size(), "cpu:0",
                                       false) == 0,
            "TCP staging registration");
    auto segment = client.openSegment(owner.getLocalIpAndPort());
    buffer.fill(73);
    mooncake::TransferRequest request{
        .opcode = mooncake::TransferRequest::WRITE,
        .source = buffer.data(),
        .target_id = segment,
        .target_offset = reinterpret_cast<uintptr_t>(target.data()),
        .length = buffer.size()};
    require(transfer(client, request).s == mooncake::Transport::COMPLETED,
            "TCP WRITE");
    buffer.fill(0);
    request.opcode = mooncake::TransferRequest::READ;
    require(transfer(client, request).s == mooncake::Transport::COMPLETED,
            "TCP READ");
    for (auto value : buffer) require(value == 73, "TCP payload");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "tcp") {
            tcpSmoke();
            std::cout << "TCP_REGRESSION_OK\n";
            return 0;
        }
        char directory[] = "/tmp/mooncake-ubsmem-te-XXXXXX";
        require(mkdtemp(directory), "directory");
        setenv("MC_UBSMEM_MOCK_DIR", directory, 1);
        setenv("MC_UBSMEM_TIMEOUT_MS", "200", 1);
        const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        const size_t chunk = 8 * page;
        setenv("MC_UBSMEM_CHUNK_SIZE", std::to_string(chunk).c_str(), 1);
        auto arena = mooncake::UbsMemoryArena::create(4 * 1024 * 1024);
        if (argc == 4) {
            mooncake::TransferEngine engine;
            require(engine.init(argv[2], "127.0.0.1:0") == 0, "HTTP init");
            require(engine.installTransport("ubsmem", nullptr),
                    "HTTP transport");
            const auto failure = std::filesystem::path(argv[3]) / "fail_put";
            if (std::string(argv[1]) == "registration") {
                std::ofstream(failure).put('1');
                require(engine.registerLocalMemory(arena->base(), arena->size(),
                                                   "cpu:0", true) != 0,
                        "injected publication failure");
                require(engine.getMetadata()
                            ->getSegmentDescByID(0)
                            ->buffers.empty(),
                        "failed registration must restore local metadata");
                std::filesystem::remove(failure);
                require(engine.registerLocalMemory(arena->base(), arena->size(),
                                                   "cpu:0", true) == 0,
                        "retry registration");
                std::ofstream(failure).put('1');
                require(engine.unregisterLocalMemory(arena->base()) != 0,
                        "injected unregistration failure");
                require(engine.getMetadata()
                                ->getSegmentDescByID(0)
                                ->buffers.size() == 1,
                        "failed unregister must retain metadata and pin");
                std::filesystem::remove(failure);
                require(engine.unregisterLocalMemory(arena->base()) == 0,
                        "retry unregister");
            } else {
                for (const auto* name : {"number", "array", "null", "string"}) {
                    require(!engine.getMetadata()->getSegmentDesc(name),
                            "malformed buffer entry must be rejected");
                }
            }
            return 0;
        }
        {
            mooncake::TransferEngine owner, client;
            owner.setAutoDiscover({.enabled = false, .protocol = "ubsmem"});
            client.setAutoDiscover({.enabled = true, .protocol = "ubsmem"});
            require(owner.init("P2PHANDSHAKE", "127.0.0.1:0") == 0,
                    "owner init");
            require(client.init("P2PHANDSHAKE", "127.0.0.1:0") == 0,
                    "client init");
            require(owner.getTransport("ubsmem"), "owner init installs ubsmem");
            require(client.getTransport("ubsmem"),
                    "client init installs ubsmem");
            require(!owner.installTransport("tcp", nullptr),
                    "no mixed transport installation");
            std::array<unsigned char, 64> buffer{};
            require(owner.registerLocalMemory(buffer.data(), buffer.size(),
                                              "cpu:0", true) != 0,
                    "cannot publish ordinary memory");
            require(client.registerLocalMemory(buffer.data(), buffer.size(),
                                               "cpu:0", false) == 0,
                    "register staging memory");
            require(owner.registerLocalMemory(arena->base(), arena->size(),
                                              "cpu:0", true) == 0,
                    "register arena");
            auto segment = client.openSegment(owner.getLocalIpAndPort());
            require(segment != static_cast<uint64_t>(-1), "open segment");
            buffer.fill(71);
            mooncake::TransferRequest request{
                .opcode = mooncake::TransferRequest::WRITE,
                .source = buffer.data(),
                .target_id = segment,
                .target_offset =
                    reinterpret_cast<uintptr_t>(arena->base()) + 37,
                .length = buffer.size()};
            auto status = transfer(client, request);
            require(status.s == mooncake::Transport::COMPLETED &&
                        status.transferred_bytes == 64,
                    "WRITE completed with exact byte count");
            buffer.fill(0);
            request.opcode = mooncake::TransferRequest::READ;
            require(
                transfer(client, request).s == mooncake::Transport::COMPLETED,
                "READ completed");
            for (auto value : buffer) require(value == 71, "payload");
            request.target_offset =
                reinterpret_cast<uintptr_t>(arena->base()) + arena->size() - 1;
            require(transfer(client, request).s == mooncake::Transport::FAILED,
                    "cross-boundary request fails");

            // A large request splits into page-aligned chunks.
            std::vector<unsigned char> payload(64 * page + 100);
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<unsigned char>(i * 7 + 3);
            }
            require(client.registerLocalMemory(payload.data(), payload.size(),
                                               "cpu:0", false) == 0,
                    "register large staging memory");
            auto big = request;
            big.opcode = mooncake::TransferRequest::WRITE;
            big.source = payload.data();
            big.length = payload.size();
            big.target_offset = reinterpret_cast<uintptr_t>(arena->base()) + 37;
            status = transfer(client, big);
            require(status.s == mooncake::Transport::COMPLETED &&
                        status.transferred_bytes == payload.size(),
                    "chunked WRITE completed with exact byte count");
            std::fill(payload.begin(), payload.end(), 0);
            big.opcode = mooncake::TransferRequest::READ;
            require(transfer(client, big).s == mooncake::Transport::COMPLETED,
                    "chunked READ completed");
            for (size_t i = 0; i < payload.size(); ++i) {
                require(payload[i] == static_cast<unsigned char>(i * 7 + 3),
                        "chunked payload");
            }

            // Stall the first chunk's pages, as a peer stalled mid-copy
            // would. Workers still copy the other chunks before it times out.
            const auto object =
                std::string(directory) + "/" + arena->descriptor().object_name;
            const int stall = open(object.c_str(), O_RDWR | O_CLOEXEC);
            struct stat file{};
            require(stall >= 0 && fstat(stall, &file) == 0, "open stall");
            struct flock first{};
            first.l_type = F_WRLCK;
            first.l_whence = SEEK_SET;
            first.l_start = file.st_size - static_cast<off_t>(arena->size());
            first.l_len = static_cast<off_t>(page);
            require(fcntl(stall, F_OFD_SETLK, &first) == 0, "stall chunk");
            big.opcode = mooncake::TransferRequest::WRITE;
            // Tasks keep a pointer to their request.
            const std::vector<mooncake::TransferRequest> pending{big};
            auto stalled = client.allocateBatchID(1);
            require(client.submitTransfer(stalled, pending).ok(),
                    "submit stalled request");
            const size_t chunks = (37 + payload.size() + chunk - 1) / chunk;
            auto& task = mooncake::Transport::toBatchDesc(stalled).task_list[0];
            require(
                __atomic_load_n(&task.slice_count, __ATOMIC_ACQUIRE) == chunks,
                "one slice per chunk");
            mooncake::TransferStatus partial{};
            const auto settle =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                require(client.getTransferStatus(stalled, 0, partial).ok(),
                        "stalled poll");
                require(std::chrono::steady_clock::now() < settle,
                        "stalled request did not finish");
            } while (partial.s == mooncake::Transport::PENDING);
            require(
                partial.s == mooncake::Transport::FAILED &&
                    __atomic_load_n(&task.success_slice_count,
                                    __ATOMIC_ACQUIRE) == chunks - 1 &&
                    partial.transferred_bytes == payload.size() - (chunk - 37),
                "only the stalled chunk fails");
            require(client.freeBatchID(stalled).ok(), "free stalled batch");
            close(stall);
            require(client.unregisterLocalMemory(payload.data()) == 0,
                    "unregister large staging memory");

            // A separate open-file description holds the mock's range check
            // over the object, as a peer stalled mid-copy would.
            const auto path =
                std::string(directory) + "/" + arena->descriptor().object_name;
            auto hold = [](int fd, short type) {
                struct flock range{};
                range.l_type = type;
                range.l_whence = SEEK_SET;
                return fcntl(fd, F_OFD_SETLKW, &range) == 0;
            };
            const int blocker = open(path.c_str(), O_RDWR | O_CLOEXEC);
            require(blocker >= 0 && hold(blocker, F_WRLCK), "block peer");
            request.target_offset = reinterpret_cast<uintptr_t>(arena->base());
            // Stay below queue capacity so submission returns while blocked.
            auto batch = client.allocateBatchID(200);
            std::vector<mooncake::TransferRequest> queued(200, request);
            require(client.submitTransfer(batch, queued).ok(), "submit queue");
            require(!client.freeBatchID(batch).ok(), "active batch is busy");
            require(client.unregisterLocalMemory(buffer.data()) != 0,
                    "active staging memory is pinned");
            std::vector<bool> done(queued.size(), false);
            size_t finished = 0;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (finished != queued.size()) {
                require(std::chrono::steady_clock::now() < deadline,
                        "blocked copies did not time out");
                for (size_t i = 0; i < queued.size(); ++i) {
                    if (done[i]) continue;
                    mooncake::TransferStatus current{};
                    require(client.getTransferStatus(batch, i, current).ok(),
                            "queue poll");
                    if (current.s == mooncake::Transport::FAILED) {
                        require(current.transferred_bytes == 0, "failed bytes");
                        done[i] = true;
                        ++finished;
                    } else {
                        require(current.s != mooncake::Transport::COMPLETED,
                                "copy bypassed stalled peer");
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(client.freeBatchID(batch).ok(), "free timed-out batch");
            require(hold(blocker, F_UNLCK), "unblock peer");
            close(blocker);
            require(
                transfer(client, request).s == mooncake::Transport::COMPLETED,
                "copy after contention clears");

            // A batch larger than the queue waits for space instead of
            // failing while a peer briefly stalls.
            const int peer = open(path.c_str(), O_RDWR | O_CLOEXEC);
            require(peer >= 0 && hold(peer, F_WRLCK), "block peer again");
            std::thread unblock([peer, hold] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                hold(peer, F_UNLCK);
            });
            auto large = client.allocateBatchID(600);
            std::vector<mooncake::TransferRequest> backlog(600, request);
            const bool submitted = client.submitTransfer(large, backlog).ok();
            unblock.join();
            close(peer);
            require(submitted, "submit batch larger than queue");
            mooncake::TransferStatus aggregate{};
            const auto drained =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                require(client.getBatchTransferStatus(large, aggregate).ok(),
                        "backlog poll");
                require(aggregate.s != mooncake::Transport::FAILED,
                        "full queue must not fail queued work");
                require(std::chrono::steady_clock::now() < drained,
                        "backlog did not drain");
            } while (aggregate.s != mooncake::Transport::COMPLETED);
            require(aggregate.transferred_bytes == backlog.size() * 64,
                    "backlog byte count");
            require(client.freeBatchID(large).ok(), "free drained backlog");
            for (int i = 0; i < 100; ++i) {
                require(transfer(client, request, i % 2 == 0).s ==
                            mooncake::Transport::COMPLETED,
                        "completion followed by immediate batch destruction");
            }
            require(owner.unregisterLocalMemory(arena->base()) == 0,
                    "unregister arena");
            require(client.unregisterLocalMemory(buffer.data()) == 0,
                    "unregister staging memory");
        }
        require(arena->remove(), "remove arena after engine shutdown");
        arena.reset();
        std::filesystem::remove_all(directory);
        std::cout << "UBSMEM_TRANSPORT_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
