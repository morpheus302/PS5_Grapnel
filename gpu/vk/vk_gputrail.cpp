// (ps5) The GPU trail turned on, fed and written out: gpu_trail.h has its rules and its ring, vk_gputrail.h the calls.
//
// (2026-11-10) The console's GPU stopped five times in one evening with nothing to say what it had been given: the queue's lock
// said who held it ("vk: the queue's lock: thread N ... has waited 5 s"), the host's watchdog where the threads were ("no frame
// for 20 s"), and that was all. The trail keeps the last submits in memory with what went into them and is written out, through
// the stage trail (hos_ps5_trail: one UDP datagram and one synced line of hookshot_stages.txt a line, from whichever thread
// asks), when:
// - a thread has waited 5 s for the queue's lock (PresentMutex::lock, vk_internal.h): the waiting thread writes it;
// - a wait for a submission has lasted 5 s (GtWait around the renderer's waits; gt_poll, called once a second by the host's
//   report thread, host/hos_thread.c, writes it: the waits themselves are left exactly as they were);
// - the host's watchdog finds no frame for 20 s (rx_ps5_vk_gpu_trail_dump from its thread).
// And once, when asked for: RX_VK_GPU_TRAIL_AT=<seconds> has it written when the run reaches that time, stall or not (unset:
// never). A stall's dump lists what the run did for the first time, and only a dump of a run that went well says what of that
// list is new.
// None of these threads is the one that is stuck, and writing takes neither the queue's lock nor the replay's: the trail's own
// spinlock for the time it takes to format the text, the driver for three vkGetSemaphoreCounterValue (which the renderer calls
// from any thread already: on the console's driver it takes the timeline's mutex and the queue's sync lock for a moment, neither
// held across the kernel's submit; the dump's first line is written before it, so a call that did not return shows).
//
// SPDX-License-Identifier: GPL-3.0-or-later (hookshot-ps5)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#if defined(__PROSPERO__) || defined(__linux__)
#include <pthread.h>   // (AskHeld below)
#include <signal.h>
#endif

#include "vk_gputrail.h"
#include "vk_internal.h"

namespace mkwvk {

std::atomic<int> gt_state{0};
gt::Trail* gt_trail = nullptr;
std::atomic<uint64_t> gt_value_seen{0};

namespace {

constexpr size_t kDumpText = 192u << 10;   // (the dump's text: at most 50 entries, 3.5 KB for one with every key it can keep)
constexpr unsigned kDumpsMost = 8;         // (whole dumps a run: the log is large already)
constexpr unsigned kRepeatsMost = 4;       // (one-line answers after a whole dump with nothing new to say)
constexpr uint64_t kDumpGapUs = 4000000;   // (and no two whole dumps within this: several threads reach their 5 s together)
constexpr uint32_t kWaitTicks = 6;         // (gt_poll's ticks, a second each: a wait told has lasted 5 s at least)

gt::Waits waits;
char* dump_text = nullptr;
std::atomic<bool> dumping{false};
uint64_t dump_at_us = 0;   // (RX_VK_GPU_TRAIL_AT: one dump at that time of the run, for reference; 0: none. Set once, in gt_ask)

uint64_t trail_now_us() {   // (the stage trail's clock on the console: its lines and the dump's times read together)
#ifdef __PROSPERO__
    return (uint64_t)(hos_ps5_since_start() * 1e6);
#else
    static const auto t0 = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
#endif
}
int trail_tid() {
    static thread_local int tid = 0;
    if (!tid) {
#ifdef __PROSPERO__
        tid = hos_ps5_tid();
#else
        tid = (int)GetCurrentThreadId();
#endif
    }
    return tid;
}
/// One line out: the stage trail on the console (the path of the queue's lock line), the log elsewhere
void trail_line(const char* line) {
#ifdef __PROSPERO__
    hos_ps5_trail("%s", line);
#else
    logf("%s", line);
#endif
}

/// (2026-11-11) The watchdog's question kept from the thread that writes a dump, for as long as it writes. At "no frame for
/// 20 s" the host asks the presentation thread and the game's main thread where they are with SIGUSR2
/// (host/posix/win32_posix.c hos_posix_ask_where), and the handler answers with a line of the stage trail. The stage trail's
/// lock is a spinlock held across a line's write and its sync, which a thread cannot take twice (host/posix/ps5_early.c
/// trail_lock), and the thread that has waited 5 s for the queue's lock is most often one of those two (8 of the 11 such
/// waits of 2026-11-10). Asked while it writes one of a dump's lines, it would wait for itself for good with that lock held,
/// and every later line of the stage trail would wait with it: the memory watch's "STOPPING THE TITLE" before its abort is
/// one. On 2026-11-10 the question came 16 to 25 s after the lock's line, and a dump is estimated at 8 s at most (193 lines
/// at most; 6 to 22 ms between two lines one thread wrote in a row that day, about 40 ms an import's line): not met with
/// those figures, and not left to them. Blocked for this thread here, the question is delivered when the dump is written,
/// and its answer says where the thread is then: in the lock's wait. (On Linux too: tests/gpu_trail_glue_test.cpp asks.)
struct AskHeld {
#if defined(__PROSPERO__) || defined(__linux__)
    sigset_t before;
    bool held;
    AskHeld() {
        // (the set that holds SIGUSR2 alone, made by taking every other signal out of a full one: sigfillset, sigdelset and
        // pthread_sigmask are calls the eboot on the console imports and makes already, at the driver's thread starts (Mesa's
        // u_thread_create); sigaddset would be one more import, which the console has never been asked for)
        sigset_t ask;
        sigfillset(&ask);
        for (int s = 1; s <= (int)(8 * sizeof ask); s++)
            if (s != SIGUSR2) sigdelset(&ask, s);
        held = pthread_sigmask(SIG_BLOCK, &ask, &before) == 0;
    }
    ~AskHeld() {
        if (held) pthread_sigmask(SIG_SETMASK, &before, nullptr);
    }
#else
    AskHeld() {}   // (no such question there)
#endif
    AskHeld(const AskHeld&) = delete;
    AskHeld& operator=(const AskHeld&) = delete;
};

} // namespace

bool gt_ask() {
    bool on;
    {
        static std::mutex mu;   // (the first calls may come from several threads at once)
        std::lock_guard lk(mu);
        if (const int s = gt_state.load(std::memory_order_acquire)) return s > 0;
        const char* e = getenv("RX_VK_GPU_TRAIL");
#ifdef __PROSPERO__
        on = !(e && e[0] == '0');
#else
        on = e && e[0] == '1';
#endif
        if (on) {
            gt_trail = new (std::nothrow) gt::Trail();
            dump_text = (char*)std::malloc(kDumpText);
            if (!gt_trail || !dump_text) {
                on = false;
                logf("vk: gpu trail: no memory for it (%zu KB): off", (sizeof(gt::Trail) + kDumpText) >> 10);
            } else {
                gt_trail->now_us = trail_now_us;
                const char* at = getenv("RX_VK_GPU_TRAIL_AT");
                if (at && atof(at) > 0) dump_at_us = (uint64_t)(atof(at) * 1e6);
            }
        }
        gt_state.store(on ? 1 : -1, std::memory_order_release);
    }
    if (on) {   // (the one line at the start, the lock of the first use let go: a synced line of the stage trail on the console)
        char line[480], at[80] = "";
        if (dump_at_us) std::snprintf(at, sizeof at, ", and once at %.0f s of the run (RX_VK_GPU_TRAIL_AT)", (double)dump_at_us / 1e6);
        std::snprintf(line, sizeof line,
                      "gpu trail: on: the last %u submits kept with their program keys (%u) and other commands (%u), %zu KB; written here "
                      "when the queue's lock or a wait for a submission has lasted 5 s, or no frame is shown for 20 s%s (RX_VK_GPU_TRAIL=0: off)",
                      gt::kEntries, gt::kKeyRing, gt::kDetRing, (sizeof(gt::Trail) + kDumpText) >> 10, at);
        trail_line(line);
    }
    return on;
}

void gt_begin_recording(int slot, const void* cmd) {
    if (!gt_on()) return;
    gt::Acc* a = (unsigned)slot < (unsigned)gt::kSlots ? &gt_trail->acc[slot] : nullptr;
    if (a) {
        a->reset();   // (a recording begun in the slot and never submitted leaves nothing behind)
        if (gt_trail->by_cmd(cmd) != a) gt_trail->bind_cmd(cmd, a);   // (once a slot: its command buffer is its own for the run)
    }
    gt_acc = a;
}

uint64_t gt_submit(gt::EntryKind kind, gt::Acc* a, int slot, uint64_t seq, uint64_t value, uint64_t signal, uint64_t batch, unsigned ncb,
                   int reason, bool call) {
    if (!gt_on()) return 0;
    gt::Entry e{};
    e.kind = kind, e.seq = seq, e.value = value, e.signal = signal, e.batch = batch;
    e.ncb = (uint16_t)(ncb > 0xFFFF ? 0xFFFF : ncb), e.slot = (uint8_t)slot, e.reason = (uint8_t)reason;
    e.tid = trail_tid(), e.call = call ? gt::CALL_IN : gt::CALL_NONE;
    const uint64_t n = gt_trail->push(e, a);
    if (a && gt_acc == a) gt_acc = nullptr;   // (what this thread notes from here on is no longer that recording's)
    return n;
}
void gt_back(uint64_t entry, int result) {
    if (entry && gt_state.load(std::memory_order_acquire) > 0) gt_trail->back(entry, result);
}

int gt_wait_begin(const char* what, uint64_t value) { return waits.begin(what, value, trail_tid()); }
void gt_wait_end(int slot) { waits.end(slot); }

void gpu_trail_dump(const char* why) {
    if (gt_state.load(std::memory_order_acquire) <= 0) return;   // (off, or never asked: nothing was recorded)
    if (dumping.exchange(true, std::memory_order_acquire)) return;   // (another thread is writing one)
    struct Done { ~Done() { dumping.store(false, std::memory_order_release); } } done;
    AskHeld ask_held;   // (the watchdog's SIGUSR2 waits until this thread has written: its handler writes to the stage trail too)
    static unsigned whole, repeats;   // (dumping: one thread at a time here)
    static uint64_t whole_at;
    static bool said_most;
    const uint64_t t = trail_now_us();
    const int tid = trail_tid();
    char line[640];
    if (whole >= kDumpsMost) {
        if (!said_most) {
            said_most = true;
            std::snprintf(line, sizeof line, "gpu trail: asked at %.3f s by thread %d (%s): %u dumps were written, no more are (the trail goes on recording)",
                          (double)t / 1e6, tid, why, whole);
            trail_line(line);
        }
        return;
    }
    const auto again = [&](const char* what) {
        if (repeats++ >= kRepeatsMost) return;
        std::snprintf(line, sizeof line, "gpu trail: asked again at %.3f s by thread %d (%s): %s dump %u, written at %.3f s%s", (double)t / 1e6, tid, why,
                      what, whole, (double)whole_at / 1e6, repeats == kRepeatsMost ? " (said no more until something moves)" : "");
        trail_line(line);
    };
    uint64_t written = 0, call = 0;
    const bool news = gt_trail->written_since_dump(&written, &call);
    const bool soon = whole && t - whole_at < kDumpGapUs;
    if (news && soon) return again("not written again so soon after");
    if (news) {   // (before the driver is asked anything: were its answer never to come, this line says what was asked and when)
        int n = std::snprintf(line, sizeof line, "gpu trail: d%u begins at %.3f s, asked by thread %d (%s): %llu entries written so far; ", whole + 1,
                              (double)t / 1e6, tid, why, (unsigned long long)written);
        if (call) n += std::snprintf(line + n, sizeof line - (size_t)n, "THE DRIVER'S CALL for entry #%llu IS NOT BACK; ", (unsigned long long)call);
        else n += std::snprintf(line + n, sizeof line - (size_t)n, "no submit is in the driver's call; ");
        std::snprintf(line + n, sizeof line - (size_t)n, "the renderer last read submission value %llu as done; the driver is asked for its timelines "
                      "next (no line of d%u after this one: that did not come back)", (unsigned long long)gt_value_seen.load(std::memory_order_relaxed),
                      whole + 1);
        trail_line(line);
    }
    gt::Now now;
    vk_trail_timelines(now);
    now.us = trail_now_us();
    if (!news && !gt_trail->moved(now)) return again("nothing was submitted and no timeline has moved since");
    if (!news && soon) return again("not written again so soon after");
    gt::Text text;
    text.reset(dump_text, kDumpText);
    text.tok("asked at %.3f s by thread %d (%s); ", (double)t / 1e6, tid, why);   // (the dump's first line goes on from here)
    gt_trail->dump(now, text);
    whole++, whole_at = t, repeats = 0;
    gt::emit(text, whole, trail_line);
}

void gt_poll() {
    if (gt_state.load(std::memory_order_acquire) <= 0) return;
    static std::atomic<bool> at_done{false};
    if (dump_at_us && trail_now_us() >= dump_at_us && !at_done.exchange(true))
        gpu_trail_dump("asked for at this time of the run, stall or not: RX_VK_GPU_TRAIL_AT");
    waits.advance();
    for (int i; (i = waits.overdue(kWaitTicks)) >= 0;) {
        const gt::WaitSlot& w = waits.slot[i];
        char why[200];
        const char* what = w.what.load(std::memory_order_relaxed);
        std::snprintf(why, sizeof why, "a wait for %s %llu has lasted 5 s on thread %d", what ? what : "a submission",
                      (unsigned long long)w.value.load(std::memory_order_relaxed), (int)w.tid.load(std::memory_order_relaxed));
        gpu_trail_dump(why);
    }
}

} // namespace mkwvk

/// (host/hos_thread.c, the console's report thread: once a second, and when no frame was shown for 20 s)
extern "C" void rx_ps5_vk_gpu_trail_poll(void) { mkwvk::gt_poll(); }
extern "C" void rx_ps5_vk_gpu_trail_dump(const char* why) { mkwvk::gpu_trail_dump(why && *why ? why : "asked"); }
