#pragma once

#include <cstdint>
#include <string>

namespace mooncake {

struct UbsMemoryDescriptor {
    uint32_t version = 1;
    std::string backend;
    std::string domain;
    std::string host;
    std::string object_name;
    std::string generation;
    uint64_t object_size = 0;
    uint64_t object_offset = 0;

    bool valid() const;
    bool operator==(const UbsMemoryDescriptor&) const = default;
};
}  // namespace mooncake
