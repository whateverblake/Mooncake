#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <thread>

#include "transport/transport.h"
#include "transport/ubsmem_transport/ubsmem_memory.h"

namespace mooncake {

class UbsMemoryTransport final : public Transport {
   public:
    UbsMemoryTransport();
    ~UbsMemoryTransport() override;
    Status freeBatchID(BatchID batch) override;
    Status submitTransfer(
        BatchID batch, const std::vector<TransferRequest>& requests) override;
    Status submitTransferTask(const std::vector<TransferTask*>& tasks) override;
    Status getTransferStatus(BatchID batch, size_t task,
                             TransferStatus& status) override;

   private:
    struct Registration {
        size_t length;
        std::shared_ptr<UbsMemoryArena> arena;
        BufferDesc descriptor;
    };
    struct Work {
        Slice* slice;
        std::shared_ptr<UbsMemoryArena> arena;
        std::shared_ptr<Registration> source;
        size_t offset;
        UbsMemoryArena::Deadline deadline;
    };

    int install(std::string& name, std::shared_ptr<TransferMetadata> metadata,
                std::shared_ptr<Topology> topology) override;
    int registerLocalMemory(void* addr, size_t length,
                            const std::string& location, bool remote,
                            bool update) override;
    int unregisterLocalMemory(void* addr, bool update) override;
    int registerLocalMemoryBatch(const std::vector<BufferEntry>& buffers,
                                 const std::string& location) override;
    int unregisterLocalMemoryBatch(
        const std::vector<void*>& addresses) override;
    const char* getName() const override { return "ubsmem"; }
    bool prepare(const TransferRequest& request, Work& work);
    void run();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::map<uintptr_t, std::shared_ptr<Registration>> registrations_;
    std::map<std::string, std::shared_ptr<UbsMemoryArena>> imports_;
    std::deque<Work> queue_;
    std::set<BatchID> uncertain_batches_;
    std::thread worker_;
    bool stopping_ = false;
    std::chrono::milliseconds timeout_{30000};
    static constexpr size_t kQueueCapacity = 256;
};
}  // namespace mooncake
