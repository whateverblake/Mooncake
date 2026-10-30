#pragma once

#include <pthread.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace mooncake {

// Runs a callback on a dedicated thread when scheduled. Multiple Schedule()
// calls made before the worker consumes the pending request are coalesced into
// one callback invocation. Start() and Stop() must be called serially by the
// owner; Schedule() may be called concurrently from other threads.
class BackgroundWorker {
   public:
    using Callback = std::function<void()>;

    // Linux thread names may contain at most 15 bytes (excluding the null).
    explicit BackgroundWorker(Callback callback, std::string thread_name = "")
        : callback_(std::move(callback)), thread_name_(std::move(thread_name)) {}

    ~BackgroundWorker() { Stop(); }

    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        requested_ = false;
        thread_ = std::thread(&BackgroundWorker::ThreadFunc, this);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !thread_.joinable()) {
                return;
            }
            running_ = false;
            requested_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Schedule() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            requested_ = true;
        }
        cv_.notify_one();
    }

   private:
    void ThreadFunc() {
        if (!thread_name_.empty()) {
            pthread_setname_np(pthread_self(), thread_name_.c_str());
        }
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_ || requested_; });
                if (!running_) {
                    return;
                }
                requested_ = false;
            }
            callback_();
        }
    }

    Callback callback_;
    std::string thread_name_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_{false};
    bool requested_{false};
};

}  // namespace mooncake
