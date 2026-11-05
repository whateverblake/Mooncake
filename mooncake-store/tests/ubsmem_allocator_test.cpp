#include "ubsmem_allocator.h"

#include <iostream>
#include <stdexcept>

int main() {
    constexpr size_t kPage = mooncake::UbsMemoryBufferAllocator::kGranularity;
    try {
        auto registry =
            std::make_shared<mooncake::UbsMemoryAllocationRegistry>();
        auto allocator = std::make_shared<mooncake::UbsMemoryBufferAllocator>(
            "owner", 0x1000000, 2 * kPage, "owner:1234", registry);
        auto first = allocator->allocate(1);
        if (!first || first->get_descriptor().protocol_ != "ubsmem") {
            throw std::runtime_error("UBS allocation descriptor");
        }
        const auto address = first->data();
        first.reset();
        auto second = allocator->allocate(1);
        if (!second || second->data() != address ||
            allocator->size() != kPage) {
            throw std::runtime_error(
                "Deleted allocation must restore capacity");
        }
        auto neighbour = allocator->allocate(1);
        const auto next_page = second->get_descriptor().buffer_address_ + kPage;
        if (!neighbour ||
            neighbour->get_descriptor().buffer_address_ != next_page) {
            throw std::runtime_error("allocations must not share a page");
        }
        neighbour.reset();
        second.reset();
        auto whole = allocator->allocate(2 * kPage);
        if (!whole || allocator->getLargestFreeRegion() != 0) {
            throw std::runtime_error("Adjacent free ranges must coalesce");
        }
        whole.reset();
        auto pinned = allocator->allocate(2 * kPage);
        const auto stale = pinned->get_descriptor();
        auto first_pin = registry->pin(stale);
        auto second_pin = registry->pin(stale);
        if (!first_pin || !second_pin)
            throw std::runtime_error("pin live range");
        auto invalid = stale;
        invalid.size_++;
        if (registry->pin(invalid))
            throw std::runtime_error("invalid descriptor");
        pinned.reset();
        if (registry->pin(stale) || allocator->allocate(1)) {
            throw std::runtime_error("retired range must stay pinned");
        }
        first_pin.reset();
        if (allocator->allocate(1)) throw std::runtime_error("second pin lost");
        second_pin.reset();
        auto reused = allocator->allocate(2 * kPage);
        if (!reused ||
            reused->get_descriptor().buffer_address_ != stale.buffer_address_ ||
            reused->get_descriptor().ubsmem_allocation_id_ ==
                stale.ubsmem_allocation_id_ ||
            registry->pin(stale)) {
            throw std::runtime_error("reuse must invalidate old allocation ID");
        }
        reused.reset();
        for (int i = 0; i < 1000; ++i) {
            if (!allocator->allocate(2 * kPage))
                throw std::runtime_error("reuse loop");
        }
        if (allocator->size() != 0 ||
            allocator->getLargestFreeRegion() != 2 * kPage) {
            throw std::runtime_error("free space accounting");
        }
        mooncake::UbsMemoryTransferRegistry transfers(registry);
        mooncake::UUID client{1, 2};
        auto live = allocator->allocate(2 * kPage);
        auto descriptor = live->get_descriptor();
        if (!transfers.begin(client, 1, descriptor) ||
            !transfers.begin(client, 1, descriptor)) {
            throw std::runtime_error("duplicate begin must be idempotent");
        }
        live.reset();
        if (allocator->allocate(1))
            throw std::runtime_error("master pin missing");
        transfers.end(client, 1);
        transfers.end(client, 1);
        auto next = allocator->allocate(2 * kPage);
        if (!next || transfers.begin(client, 1, next->get_descriptor())) {
            throw std::runtime_error("ended operation must not reopen");
        }
        transfers.end(client, 3);
        transfers.end(client, 2);
        if (transfers.begin(client, 2, next->get_descriptor()) ||
            transfers.begin(client, 3, next->get_descriptor()) ||
            !transfers.begin(client, 4, next->get_descriptor())) {
            throw std::runtime_error("out-of-order end handling");
        }
        transfers.end(client, 4);
        std::cout << "UBSMEM_ALLOCATOR_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
