#include "../src/background_worker.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
using minimal_taskbar_monitor::BackgroundWorker;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestCoalescingAndCancellation() {
    BackgroundWorker<int, int> worker;
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    auto completed = std::make_shared<std::promise<void>>();
    auto completed_future = completed->get_future();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto notifications = std::make_shared<std::atomic<int>>(0);
    worker.Start([entered, released, calls](const int& value, const auto&) {
        ++*calls;
        if (value == 1) {
            entered->set_value();
            released.wait_for(3s);
        }
        return value;
    }, [completed, notifications] {
        if (++*notifications == 1) completed->set_value();
    });
    Require(worker.Submit(L"slow", 1), "First job must start");
    Require(entered_future.wait_for(2s) == std::future_status::ready, "Worker did not start");
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        Require(!worker.Submit(L"slow", 1), "Timer requests must coalesce");
    }
    worker.Submit(L"middle", 2);
    worker.Submit(L"latest", 3);
    Require(std::chrono::steady_clock::now() - start < 250ms, "UI submission blocked on slow work");
    release->set_value();
    Require(completed_future.wait_for(2s) == std::future_status::ready, "Latest job never completed");
    const auto result = worker.TakeResult();
    Require(result && *result == 3 && *calls == 2 && *notifications == 1,
            "Only the latest selection should run/publish after an obsolete slow request");
    Require(!worker.Busy(), "Worker stayed busy after result consumption");
    worker.Stop();
}

void TestNonBlockingShutdown() {
    BackgroundWorker<int, int> worker;
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    auto finished = std::make_shared<std::promise<void>>();
    auto done = finished->get_future();
    auto notices = std::make_shared<std::atomic<int>>(0);
    worker.Start([entered, released, finished](const int&, const auto& cancelled) {
        entered->set_value();
        released.wait_for(3s);
        const bool stopped = cancelled();
        finished->set_value();
        return stopped ? 1 : 0;
    }, [notices] { ++*notices; });
    worker.Submit(L"slow", 1);
    Require(started.wait_for(2s) == std::future_status::ready, "Shutdown test did not start");
    const auto start = std::chrono::steady_clock::now();
    worker.Stop();
    Require(std::chrono::steady_clock::now() - start < 250ms, "Shutdown waited for network");
    release->set_value();
    Require(done.wait_for(2s) == std::future_status::ready, "Stopped job did not finish");
    Require(*notices == 0 && !worker.TakeResult(), "Closed UI must not receive callbacks");
}
}  // namespace

int main() {
    try {
        TestCoalescingAndCancellation();
        TestNonBlockingShutdown();
        std::cout << "Slow requests, coalescing, rapid switching, stale results and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
