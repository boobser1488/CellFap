#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#ifdef __ANDROID__
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
inline void cpuRelax() { _mm_pause(); }
#elif defined(__aarch64__) || defined(__arm__)
// The CPU's spin-wait hint, not a system call: std::this_thread::yield() in a spin loop made thousands of syscalls
// per phase on the phone.
inline void cpuRelax() { __asm__ __volatile__("yield"); }
#else
inline void cpuRelax() { std::this_thread::yield(); }
#endif

// Fork-join pool tuned for many short parallel phases per frame.
//
// Workers spin on an atomic generation counter for a while before sleeping on a condition
// variable: waking sleeping threads through the OS can cost a full scheduler quantum (~15 ms on
// Windows), which would dwarf a sub-millisecond simulation phase.
//
// Publishing uses a seqlock: the generation is odd while the job descriptor is being written and
// even once it is ready; workers validate the generation before and after copying the descriptor.
class JobPool {
public:
    explicit JobPool(unsigned threads = 0) {
        // Leave one hardware thread to the OS / other apps: a descheduled worker holding a chunk stalls the
        // whole phase, which hurts far more than one fewer worker.
        // A phone also keeps a core free for the interface thread (the dish thread is busy stepping): with every core
        // taken the UI stuttered while incubating.
#ifdef __ANDROID__
        // hardware_concurrency() counts only the cores online right now (idle ones are switched off): the pool started
        // with 2 threads on an 8-core phone. Count all the configured ones.
        if (threads == 0) threads = (unsigned)std::max(1L, sysconf(_SC_NPROCESSORS_CONF) - 2);
#else
        if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency() - 1);
#endif
        threads = std::min(threads, 32u);
        for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
    }
    ~JobPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_.store(true);
            gen_.fetch_add(2);
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    unsigned threadCount() const { return (unsigned)workers_.size() + 1; }

    // f(begin, end) is called for disjoint ranges covering [0, n). Not reentrant.
    template <class F>
    void parallelFor(int n, int grain, F&& f) {
        if (n <= 0) return;
        // At least `grain` items per chunk, but ~8 chunks per thread so stragglers (a slow little core of a phone holding
        // the last chunk) can be balanced.
        grain = std::max(std::max(1, grain), n / (int)(threadCount() * 8));
        if (workers_.empty() || n <= grain) {
            f(0, n);
            return;
        }
        using Fn = std::remove_reference_t<F>;
        Job job;
        job.call = [](void* ctx, int b, int e) { (*static_cast<Fn*>(ctx))(b, e); };
        job.ctx = (void*)&f;
        job.n = n;
        job.grain = grain;

        gen_.fetch_add(1);  // odd: publishing
        // Workers still inside the previous job must leave before its descriptor is replaced.
        while (active_.load() > 0) cpuRelax();
        job_ = job;
        next_.store(0, std::memory_order_relaxed);
        pending_.store((n + grain - 1) / grain, std::memory_order_relaxed);
        gen_.fetch_add(1);  // even: ready
        if (sleepers_.load() > 0) {
            std::lock_guard<std::mutex> lk(m_);
            cv_.notify_all();
        }
        runChunks(job);
        for (int spins = 0; pending_.load(std::memory_order_acquire) > 0; ++spins) {
            if (spins < 512) cpuRelax();
            else std::this_thread::yield();  // let a preempted worker finish its chunk
        }
    }

private:
    struct Job {
        void (*call)(void*, int, int) = nullptr;
        void* ctx = nullptr;
        int n = 0, grain = 1;
    };

    void runChunks(const Job& job) {
        for (;;) {
            int b = next_.fetch_add(job.grain, std::memory_order_relaxed);
            if (b >= job.n) break;
            job.call(job.ctx, b, std::min(job.n, b + job.grain));
            pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    void workerLoop() {
        uint64_t seen = 0;
        int spins = 0;
        for (;;) {
            uint64_t g = gen_.load();
            if (quit_.load()) return;
            if (g == seen || (g & 1)) {
                ++spins;
                if (spins < kSpinLimit) {
                    cpuRelax();
                } else if (spins < kYieldLimit) {
                    std::this_thread::yield();
                } else {
                    std::unique_lock<std::mutex> lk(m_);
                    sleepers_.fetch_add(1);
                    cv_.wait(lk, [&] {
                        uint64_t x = gen_.load();
                        return quit_.load() || (x != seen && !(x & 1));
                    });
                    sleepers_.fetch_sub(1);
                    spins = 0;
                }
                continue;
            }
            seen = g;
            active_.fetch_add(1);
            if (gen_.load() != g) {
                active_.fetch_sub(1);
                continue;
            }
            Job job = job_;
            if (gen_.load() != g) {  // descriptor was being replaced while we copied it
                active_.fetch_sub(1);
                continue;
            }
            runChunks(job);
            active_.fetch_sub(1);
            spins = 0;
        }
    }

    static constexpr int kSpinLimit = 1 << 11;   // busy-wait briefly (phases come in bursts)
    static constexpr int kYieldLimit = 1 << 13;  // then yield, then sleep on the condition variable

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> quit_{false};
    std::atomic<int> sleepers_{0};
    std::atomic<int> active_{0};
    Job job_;
    std::atomic<int> next_{0};
    std::atomic<int> pending_{0};
};
