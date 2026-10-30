#pragma once

#include <cstring>
#include <string_view>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace mooncake {

// Best-effort debugger labels. Linux allows 15 bytes plus the terminator.
inline void SetCurrentThreadName(std::string_view name) {
#if defined(__linux__) || defined(__APPLE__)
    char label[16] = {};
    const size_t size = name.size() < sizeof(label) - 1
                            ? name.size()
                            : sizeof(label) - 1;
    if (size == 0) return;
    std::memcpy(label, name.data(), size);
#if defined(__APPLE__)
    (void)pthread_setname_np(label);
#else
    (void)pthread_setname_np(pthread_self(), label);
#endif
#else
    (void)name;
#endif
}

}  // namespace mooncake
