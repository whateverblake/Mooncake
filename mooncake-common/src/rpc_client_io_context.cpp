#include "rpc_client_io_context.h"
#include "thread_name.h"

namespace mooncake {

namespace {
class NamedRpcClientIoContextPool : public coro_io::io_context_pool {
   public:
    using coro_io::io_context_pool::io_context_pool;

    void run() {
        SetCurrentThreadName("mc-rpc-cli-mgr");
        coro_io::io_context_pool::run();
    }
};
}  // namespace

void SetRpcIoThreadNames(coro_io::io_context_pool& pool,
                        const std::string& name_prefix) {
    size_t index = 0;
    for (auto& executor : pool.get_all_executor()) {
        executor->schedule(
            [name = name_prefix + "-" + std::to_string(index++)] {
                SetCurrentThreadName(name);
            });
    }
}

std::shared_ptr<coro_io::io_context_pool> CreateRpcClientIoContextPool(
    uint32_t thread_count) {
    auto pool =
        coro_io::create_io_context_pool<NamedRpcClientIoContextPool>(thread_count);
    SetRpcIoThreadNames(*pool, "mc-rpc-cli");
    return pool;
}

}  // namespace mooncake
