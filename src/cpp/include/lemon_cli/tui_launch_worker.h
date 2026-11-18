#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace lemon_cli {

class LaunchWorker {
public:
    LaunchWorker() : state_(std::make_shared<State>()) {
        std::thread([state = state_] {
            while (true) {
                Work work;
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->ready.wait(lock, [&] { return state->stopping || !state->tasks.empty(); });
                    if (state->stopping) return;
                    work = std::move(state->tasks.front());
                    state->tasks.pop_front();
                }
                work.run();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->stopping) return;
                    state->results.push_back(std::move(work.complete));
                }
                state->ready.notify_all();
            }
        }).detach();
    }

    LaunchWorker(const LaunchWorker&) = delete;
    LaunchWorker& operator=(const LaunchWorker&) = delete;
    ~LaunchWorker() { stop(); }

    // Work owns its inputs/results; only completions may reference UI state.
    void submit(std::function<void()> run, std::function<void()> complete, bool priority = false) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->stopping) return;
            Work work{std::move(run), std::move(complete)};
            if (priority) state_->tasks.push_front(std::move(work));
            else state_->tasks.push_back(std::move(work));
        }
        state_->ready.notify_all();
    }

    bool drain() {
        std::deque<std::function<void()>> results;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            results.swap(state_->results);
        }
        for (auto& complete : results) complete();
        return !results.empty();
    }

    void wait_for_results(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->ready.wait_for(lock, delay, [&] { return state_->stopping || !state_->results.empty(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopping = true;
            state_->tasks.clear();
            state_->results.clear();
        }
        state_->ready.notify_all();
    }

private:
    struct Work {
        std::function<void()> run;
        std::function<void()> complete;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Work> tasks;
        std::deque<std::function<void()>> results;
        bool stopping = false;
    };
    std::shared_ptr<State> state_;
};

} // namespace lemon_cli
