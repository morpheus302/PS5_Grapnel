// The worker threads (vk_jobs.h). As many as a third of the hardware threads (2 to 4): the game itself runs six
// virtual cores, and the replay thread is one of the busiest.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_jobs.h"
#include "longshot_env.h"
#include "vk_internal.h"
#include "vk_draw.h"   // (ub_thread_off)

namespace mkwvk {
namespace {

std::mutex mu;
std::condition_variable work_cv, done_cv;
struct Entry {
    std::function<void()> fn;
    const std::atomic<bool>* flag;   // (what the job sets when done, for jobs_wait to find it while queued)
};
std::deque<Entry> queue;
std::atomic<int> pending{0};
bool started = false;

// (a job that throws: the game ended at once ("approach Malon, crash": an exception escaped a worker, KERNELBASE's RaiseException
// with no handler, 2026-11-08). Resource jobs publish failure before the exception is logged, so their waiters finish.)
void run_guarded(std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed: %s", e.what());
    } catch (...) {
        static std::atomic<int> logged;
        if (logged++ < 20) logf("vk: a background job failed (not a std::exception)");
    }
}

// Wrap before queueing: shared workers, the serial worker and jobs_wait's stolen jobs all run the same recovery.
// The resource owns its failure representation (failed program, null pipeline, completed compute state).
std::function<void()> with_failure(std::function<void()> fn, std::function<void()> on_failure) {
    if (!on_failure) return fn;
    return [fn = std::move(fn), on_failure = std::move(on_failure)] {
        try {
            fn();
        } catch (...) {
            on_failure();
            throw;   // run_guarded still logs the original exception
        }
    };
}

void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"mkw_vk worker");
    ub_thread_off();
    for (;;) {
        std::function<void()> fn;
        VK_CPU_MARK(1);   // (ps5, lot 40: HOOKSHOT_PS5_THREAD_TIMES; outside the queue's lock)
        {
            std::unique_lock lk(mu);
            work_cv.wait(lk, [] { return !queue.empty(); });
            fn = std::move(queue.front().fn);
            queue.pop_front();
        }
        VK_CPU_MARK(0);
        run_guarded(fn);
        pending--;
        jobs_notify();
    }
}

void start() {
    // (longshot: with 16 hardware threads or more, 6 workers; RX_VK_WORKERS=<n>: n, 1 to 16)
    // (ps5, 2026-11-10) the console keeps the old default (2 to 4: each one translating or compiling a large program holds
    // tens of MiB of the heap meanwhile; the title's heap rose by 436 MiB in four seconds of play where 110 pipelines were
    // made), and its settings file says RX_VK_WORKERS=2. CompileThread: the console's workers with a large stack
    const unsigned hw = std::thread::hardware_concurrency();
#ifdef __PROSPERO__
    unsigned n = std::clamp(hw / 3, 2u, 4u);
#else
    unsigned n = hw >= 16 ? 6u : std::clamp(hw / 3, 2u, 4u);
#endif
    if (const char* e = getenv("RX_VK_WORKERS")) n = (unsigned)std::clamp(atoi(e), 1, 16);
    for (unsigned i = 0; i < n; i++) CompileThread(worker).detach();
    started = true;
    logf("vk: %u background workers (shader translation, pipelines)", n);
}

} // namespace

void jobs_push(std::function<void()> fn, bool urgent, const std::atomic<bool>* flag, std::function<void()> on_failure) {
    fn = with_failure(std::move(fn), std::move(on_failure));
    std::lock_guard lk(mu);
    if (!started) start();
    pending++;
    if (urgent) queue.push_front({std::move(fn), flag});
    else queue.push_back({std::move(fn), flag});
    work_cv.notify_one();
}
// (issue 496) Intel A750: four of the game's big compute pipelines built at once by Intel's compiler (up to 58 s each) froze
// the whole PC. One thread builds them one at a time there, below normal priority, beside the shared workers.
// (longshot) Between two of its builds no compute build is under way: the moment Intel's pipeline cache fetch waits for
// (pipeline_cache_between_builds). first: the front lane, in order among its own
void jobs_push_serial(std::function<void()> fn, std::function<void()> on_failure, bool first) {
    fn = with_failure(std::move(fn), std::move(on_failure));
    static std::mutex smu;
    static std::condition_variable scv;
    static std::deque<std::function<void()>> sq, sq_first;
    static bool sstarted = false;
    std::lock_guard lk(smu);
    if (!sstarted) {
        sstarted = true;
        CompileThread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SetThreadDescription(GetCurrentThread(), L"mkw_vk serial builds");
            ub_thread_off();
            for (;;) {
                std::function<void()> fn;
                VK_CPU_MARK(1);
                {
                    std::unique_lock lk(smu);
                    scv.wait(lk, [] { return !sq.empty() || !sq_first.empty(); });
                    auto& q = sq_first.empty() ? sq : sq_first;
                    fn = std::move(q.front());
                    q.pop_front();
                }
                VK_CPU_MARK(0);
                run_guarded(fn);
                jobs_notify();
                std::function<void()> between = pipeline_cache_between_builds;
                run_guarded(between);
            }
        }).detach();
        logf("vk: compute pipelines are built one at a time (the game's dispatches before the mesh fallback's)");
    }
    (first ? sq_first : sq).push_back(std::move(fn));
    scv.notify_one();
}
void jobs_notify() {
    std::lock_guard lk(mu);
    done_cv.notify_all();
}
bool jobs_wait(const std::atomic<bool>& flag) {
    std::unique_lock lk(mu);
    // (still queued: run here, now. A draw that must wait for its pipeline waited for every job queued before it, and a new
    // area queues a hundred pipelines: with ten workers at 0.3-2 s a build, frames stopped for 6-12 s, the draw's own build
    // still in the queue; Mako's RX 6600 log, one draw 118 s behind 26 queued jobs. Taken out of the queue under the lock a
    // worker takes jobs under, so it runs once, here or on a worker, and nothing is left queued for it. RX_VK_JOB_STEAL=0
    // (or off, false, no: longshot_env.h's rule): waited for in turn, as before)
    // (ps5) Off on the console unless RX_VK_JOB_STEAL=1: the waiting thread (the replay's, the present's) would translate and
    // compile there, on a stack that is not a CompileThread's 8 MiB (the driver's compiles are deep), and its heap would hold a
    // compile's tens of MiB beside the workers' (RX_VK_WORKERS=2 on the console); it waits for the worker, as before
    static const bool steal = [] {
        const char* v = getenv("RX_VK_JOB_STEAL");
#ifdef __PROSPERO__
        return v && !env_value_off(v);
#else
        return !v || !env_value_off(v);
#endif
    }();
    bool ran = false;
    if (steal && !flag.load(std::memory_order_acquire))
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (it->flag == &flag) {
                std::function<void()> fn = std::move(it->fn);
                queue.erase(it);
                lk.unlock();
                run_guarded(fn);
                pending--;
                jobs_notify();
                lk.lock();
                ran = true;
                break;
            }
    done_cv.wait(lk, [&] { return flag.load(std::memory_order_acquire); });
    return ran;
}
bool jobs_hurry(const std::atomic<bool>& flag) {
    std::lock_guard lk(mu);
    if (flag.load(std::memory_order_acquire)) return false;
    for (auto it = queue.begin(); it != queue.end(); ++it)
        if (it->flag == &flag) {
            if (it != queue.begin()) {
                Entry e = std::move(*it);
                queue.erase(it);
                queue.push_front(std::move(e));
            }
            return true;
        }
    return false;
}
// Waits at most ms for flag, without running a queued job on this thread. (ps5, 2026-11-12) The job is not run here: a build
// the waiting thread took could not be left when the time is up (the driver compiled one pipeline for up to 2.7 s on the
// console). Still queued, it goes to the front instead, so the next free worker takes it
bool jobs_wait_for(const std::atomic<bool>& flag, double ms) {
    std::unique_lock lk(mu);
    if (flag.load(std::memory_order_acquire)) return true;
    for (auto it = queue.begin(); it != queue.end(); ++it)
        if (it->flag == &flag) {
            if (it != queue.begin()) {
                Entry e = std::move(*it);
                queue.erase(it);
                queue.push_front(std::move(e));
            }
            break;
        }
    if (ms <= 0) return false;
    return done_cv.wait_for(lk, std::chrono::duration<double, std::milli>(ms),
                            [&] { return flag.load(std::memory_order_acquire); });
}
int jobs_pending() {
    return pending.load();
}

} // namespace mkwvk
