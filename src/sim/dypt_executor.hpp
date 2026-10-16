#pragma once
// The thread every dypt call of one owner runs on, with a watchdog.
//
// dypt has no instruction limit of its own and its scripts are not thread-safe: each owner (the program cells of a
// dish, the mods) keeps its scripts on one executor thread and waits for every call with a timeout. A call that does
// not return in time cannot be stopped from outside: the executor is abandoned (detached — it keeps one core busy
// until the game ends) and the owner lets go of the scripts that thread owned. The next call starts a fresh executor.
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class DyptExecutor {
public:
    DyptExecutor() = default;
    ~DyptExecutor() { stop(); }
    DyptExecutor(const DyptExecutor&) = delete;
    DyptExecutor& operator=(const DyptExecutor&) = delete;

    // Runs fn on the executor and waits up to timeoutMs. false: it hung and the executor was abandoned. fn must keep
    // everything it touches alive by itself (shared_ptr): after a timeout it may still run long after this returns.
    bool run(std::function<void()> fn, double timeoutMs) {
        if (!state_) start();
        std::shared_ptr<State> st = state_;
        std::unique_lock<std::mutex> lk(st->m);
        st->job = std::move(fn);
        st->hasJob = true;
        st->done = false;
        st->cv.notify_all();
        if (st->cv.wait_for(lk, std::chrono::duration<double, std::milli>(timeoutMs), [&] { return st->done; })) return true;
        lk.unlock();
        if (st->thread.joinable()) st->thread.detach();
        state_.reset();
        abandoned_ = true;
        return false;
    }
    // Whether a thread is running (scripts made on it can be closed on it).
    bool running() const { return state_ != nullptr; }
    // Some executor was abandoned inside a script: the dypt library must never be unloaded.
    bool everAbandoned() const { return abandoned_; }
    // Ends an idle executor.
    void stop() {
        if (!state_) return;
        {
            std::lock_guard<std::mutex> lk(state_->m);
            state_->quit = true;
        }
        state_->cv.notify_all();
        if (state_->thread.joinable()) state_->thread.join();
        state_.reset();
    }

private:
    struct State {
        std::thread thread;
        std::mutex m;
        std::condition_variable cv;
        std::function<void()> job;
        bool hasJob = false, done = false, quit = false;
    };
    void start() {
        auto st = std::make_shared<State>();
        st->thread = std::thread([st] {
            std::unique_lock<std::mutex> lk(st->m);
            for (;;) {
                st->cv.wait(lk, [&] { return st->quit || st->hasJob; });
                if (st->quit) return;
                std::function<void()> job = std::move(st->job);
                st->hasJob = false;
                lk.unlock();
                job();
                job = nullptr;  // the job's state goes before the caller is told
                lk.lock();
                st->done = true;
                st->cv.notify_all();
            }
        });
        state_ = st;
    }
    std::shared_ptr<State> state_;
    bool abandoned_ = false;
};
