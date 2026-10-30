#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace minimal_taskbar_monitor {

// One running job and at most one pending job. New selections replace pending
// work; identical timer requests are coalesced. Only value snapshots cross threads.
template <typename Input, typename Output>
class BackgroundWorker {
public:
    using Cancelled = std::function<bool()>;
    using Process = std::function<Output(const Input&, const Cancelled&)>;

    BackgroundWorker() = default;
    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;
    ~BackgroundWorker() { Stop(); }

    void Start(Process process, std::function<void()> notify) {
        Stop();
        state_ = std::make_shared<State>();
        state_->notify = std::move(notify);
        thread_ = std::thread([state = state_, process = std::move(process)] {
            for (;;) {
                std::optional<Job> job;
                {
                    std::unique_lock lock(state->mutex);
                    state->ready.wait(lock, [&] { return state->stopped || state->pending; });
                    if (state->stopped) {
                        return;
                    }
                    job = std::move(state->pending);
                    state->pending.reset();
                    state->running_key = job->key;
                    state->running_version = job->version;
                }
                const Cancelled cancelled = [state, version = job->version] {
                    return state->version.load() != version;
                };
                std::optional<Output> output;
                try {
                    output = process(job->input, cancelled);
                } catch (...) {
                    // A failed sample must not terminate the application or strand
                    // the single-flight flag. The next timer can retry it.
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->running_key.clear();
                    if (!cancelled()) {
                        state->result = std::move(output);
                        if (state->notify) {
                            state->notify();
                        }
                    }
                }
            }
        });
    }

    bool Submit(std::wstring key, Input input) {
        if (!state_) {
            return false;
        }
        std::lock_guard lock(state_->mutex);
        if (state_->stopped || (state_->pending && state_->pending->key == key) ||
            (state_->running_key == key && state_->running_version == state_->version.load()) ||
            state_->result) {
            return false;
        }
        const auto version = ++state_->version;
        state_->pending = Job{std::move(key), std::move(input), version};
        state_->ready.notify_one();
        return true;
    }

    std::optional<Output> TakeResult() {
        if (!state_) {
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        auto result = std::move(state_->result);
        state_->result.reset();
        return result;
    }

    bool Busy() const {
        if (!state_) {
            return false;
        }
        std::lock_guard lock(state_->mutex);
        return state_->pending.has_value() || state_->result.has_value() ||
               (!state_->running_key.empty() &&
                state_->running_version == state_->version.load());
    }

    void Cancel() {
        if (!state_) {
            return;
        }
        std::lock_guard lock(state_->mutex);
        ++state_->version;
        state_->pending.reset();
        state_->result.reset();
    }

    void Stop() {
        if (!state_) {
            return;
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->stopped = true;
            ++state_->version;
            state_->pending.reset();
            state_->result.reset();
            state_->notify = {};
        }
        state_->ready.notify_one();
        // OS/network sampling can finish after the UI closes. The thread owns
        // its state and sampler; it never captures the app, HWND owner, or UI data.
        // Disabling notify under the lock prevents posts to a destroyed window.
        if (thread_.joinable()) {
            thread_.detach();
        }
        state_.reset();
    }

private:
    struct Job {
        std::wstring key;
        Input input;
        std::uint64_t version;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::atomic<std::uint64_t> version{0};
        bool stopped{false};
        std::wstring running_key;
        std::uint64_t running_version{0};
        std::optional<Job> pending;
        std::optional<Output> result;
        std::function<void()> notify;
    };
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace minimal_taskbar_monitor
