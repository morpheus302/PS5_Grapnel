// (ps5) The GPU trail's glue run off the console: gpu/vk/vk_gputrail.cpp itself, compiled with this file in place of the
// renderer (tests/test_gpu_trail.py builds and runs it where the Vulkan headers are). The log stands for the stage trail, a
// fake for the driver's timelines. Run four times: with RX_VK_GPU_TRAIL unset and 0 (nothing made, nothing said), with 1, and
// with 1 and RX_VK_GPU_TRAIL_AT ("at": one dump when the run reaches that time, with no stall).
// (2026-11-11) The log here also stands for the stage trail's lock, which a thread cannot take twice, and SIGUSR2 for the host's
// watchdog asking a thread where it is (its handler writes a stage-trail line): asked while it writes a dump's line, the thread
// would wait for itself for good. The first dump below is written with the question sent at every line.
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "vk_gputrail.h"
#include "vk_internal.h"

// (the watchdog's question, host/posix/win32_posix.c: SIGUSR2, answered by a line of the stage trail)
static volatile sig_atomic_t in_line, ask_in_lines, answers, answers_in_a_line;
static void on_ask(int) {
    answers = answers + 1;
    if (in_line) answers_in_a_line = answers_in_a_line + 1;   // (on the console: the stage trail's lock asked for by the thread that holds it)
}
namespace mkwvk {
static std::vector<std::string> lines;
void logf(const char* fmt, ...) {
    char b[1024];   // (gpu/vk/vk_main.cpp logf's)
    std::va_list a;
    va_start(a, fmt);
    const int n = std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n >= (int)sizeof b) std::printf("FAIL a line does not fit the log's 1,024 bytes\n"), std::exit(1);
    in_line = 1;   // (the stage trail's lock taken: held across the line's write and its sync)
    if (ask_in_lines) pthread_kill(pthread_self(), SIGUSR2);   // (not blocked: its handler runs before this returns)
    lines.push_back(b);
    in_line = 0;
}
static gt::Now driver;
static int asked;
void vk_trail_timelines(gt::Now& now) {
    asked++;
    now.value_done = driver.value_done, now.rec_done = driver.rec_done, now.batch_done = driver.batch_done, now.handed = driver.handed;
}
} // namespace mkwvk
extern "C" DWORD GetCurrentThreadId(void) { return 77; }

using namespace mkwvk;
static int failed;
#define CHECK(cond, what) \
    do { \
        if (!(cond)) std::printf("FAIL %s (%s:%d: %s)\n", what, __FILE__, __LINE__, #cond), failed++; \
        else std::printf("PASS %s\n", what); \
    } while (0)
static size_t count(const char* what) {
    size_t n = 0;
    for (const std::string& l : lines) n += l.find(what) != std::string::npos;
    return n;
}

static uint64_t frame(int f, bool back) {   // (two recordings held for a batch and its flush, as submit_recording and queue_flush_locked write them)
    static char cb[8];   // (addresses to stand for the slots' command buffers)
    for (int r = 0; r < 2; r++) {
        gt_begin_recording(r, &cb[r]);
        for (int d = 0; d < 50; d++) gt_draw(&cb[r], 0x528c39df51bae6e5ull + (uint64_t)(d / 10), d == 7 ? gt::K_FALLBACK : gt::K_DRAW);
        gt_dispatch(&cb[r], 0x53509044401d78d2ull, gt::K_DISPATCH, 64, 36, 1);
        gt_note(gt::CLEAR_ATT_COLOR, 2, 97, 0, 1536, 864, 1);
        if (f == 1 && r == 0) gt_note(gt::UP_ZERO_COLOR, 3, 122, 0x3f, 128, 72, 96);
        gt_submit(gt::E_REC, gt_slot(r), r, (uint64_t)(f * 2 - 1 + r), 1000 + (uint64_t)f, 0, (uint64_t)f, 2, 0, false);
    }
    gt_note_num(gt::IMG_DESTROY, 999 + (uint64_t)f);   // (outside any recording: with the flush)
    const uint64_t e = gt_submit(gt::E_FLUSH, nullptr, 0, (uint64_t)f * 2, 0, 1000 + (uint64_t)f, (uint64_t)f, 4, 4, true);
    if (back) gt_back(e, 0);
    return e;
}

int main(int argc, char** argv) {
    const bool want_on = argc > 1 && !std::strcmp(argv[1], "on");
    // before anything asks: a dump or a tick does nothing, and does not ask
    gpu_trail_dump("too early");
    gt_poll();
    CHECK(gt_state.load() == 0 && !gt_trail && lines.empty(), "a dump or a tick before the first use does nothing and does not read the switch");
    gt_draw(&failed, 1, gt::K_DRAW);
    CHECK(gt_state.load() == 0, "nor does a draw");
    frame(1, true);
    if (!want_on) {
        frame(2, false);
        { GtWait w("submission value", 5); for (int i = 0; i < 8; i++) gt_poll(); }
        gpu_trail_dump("the queue's lock has been waited for 5 s");
        GtScope scope;
        CHECK(gt_state.load() == -1 && !gt_trail && !gt_acc && !scope.on && scope.submit(gt::E_PRESENT, 1) == 0, "off: the switch says so, no trail is made, a recording has no place");
        CHECK(lines.empty() && asked == 0, "off: no line is written and the driver is asked nothing");
        return failed != 0;
    }
    CHECK(gt_state.load() == 1 && gt_trail && lines.size() == 1 && lines[0].rfind("gpu trail: on: the last 1024 submits kept", 0) == 0 &&
              lines[0].find("(RX_VK_GPU_TRAIL=0: off)") != std::string::npos, "on: one line at the first use says so, with the trail's size");
    if (argc > 2 && !std::strcmp(argv[2], "at")) {   // (RX_VK_GPU_TRAIL_AT=0.05: asked for at 50 ms of the run)
        CHECK(lines[0].find(", and once at 0 s of the run (RX_VK_GPU_TRAIL_AT)") != std::string::npos, "the line says a dump is asked for at a time");
        lines.clear();
        driver.value_done = 1001, driver.rec_done = 2, driver.batch_done = 1, driver.handed = 1001;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        gt_poll();
        CHECK(count("gpu trail: d1 begins at ") == 1 && count("(asked for at this time of the run, stall or not: RX_VK_GPU_TRAIL_AT)") == 2 &&
                  count("every entry kept is done or not known") == 1 && count("upload:zero/ClearColorImage 3D fmt 122/0x3f: x1") == 1,
              "at that time the watcher has the trail written, with the run's first uses, though nothing is stuck");
        const size_t n = lines.size();
        for (int i = 0; i < 5; i++) gt_poll();
        CHECK(lines.size() == n, "once");
        std::printf(failed ? "FAILED: %d\n" : "PASSED: 0 failed\n", failed);
        return failed != 0;
    }
    CHECK(lines[0].find("RX_VK_GPU_TRAIL_AT") == std::string::npos, "and names no time when none is asked for");
    CHECK(!gt_acc && gt_trail->n_entries == 3 && gt_trail->entries[0].draws == 50 && gt_trail->entries[0].key_n == 6 && gt_trail->entries[2].det_n == 1,
          "a frame's two recordings and its flush are three entries, with their keys and notes; the thread records nowhere after its submit");
    lines.clear();
    frame(2, true);
    const uint64_t stuck = frame(3, false);   // (the driver never returns from this flush)
    driver.value_done = 1001, driver.rec_done = 2, driver.batch_done = 1, driver.handed = 1003;
    std::signal(SIGUSR2, on_ask);
    ask_in_lines = 1;   // (the watchdog asks this thread where it is while it writes each line of the dump)
    gpu_trail_dump("the queue's lock has been waited for 5 s");
    ask_in_lines = 0;
    CHECK(lines.size() > 8 && answers_in_a_line == 0 && answers == 1,
          "the watchdog's question sent to the thread that writes a dump is answered once the dump is written, never inside one of its lines");
    CHECK(asked == 1 && lines.size() > 8 && lines[0].rfind("gpu trail: d1 begins at ", 0) == 0 &&
              lines[0].find("asked by thread 77 (the queue's lock has been waited for 5 s): 9 entries written so far; THE DRIVER'S CALL for entry #9 IS NOT BACK") !=
                  std::string::npos, "the dump's first line says who asks, why, and that a call into the driver is not back");
    CHECK(stuck == 9 && lines[1].rfind("gpu trail: d1 1/", 0) == 0 && lines[1].find("asked at ") != std::string::npos &&
              lines[1].find("batches done to 1 of 3 sent") != std::string::npos && lines[1].find("6 entries not done, the first #4 rec 3 (batch 2)") != std::string::npos,
          "the next one gives the timelines and the first entry not done");
    CHECK(count("upload:zero/ClearColorImage 3D fmt 122/0x3f: x1") == 1 && count("#4 rec 3 NOT DONE: batch 2") == 1 && count("#9 flush CALL NOT BACK: batch 3") == 1 &&
              count("image:destroyed (used up to value 1002) x1") == 1 && count("end of the dump: the first entry not done is #4") == 1,
          "then the run's first uses and the entries, to the last line");
    bool numbered = true;
    size_t longest = 0;
    for (size_t i = 1; i < lines.size(); i++) {
        char want[64];
        std::snprintf(want, sizeof want, "gpu trail: d1 %zu/%zu: ", i, lines.size() - 1);
        numbered = numbered && lines[i].rfind(want, 0) == 0;
        longest = std::max(longest, lines[i].size());
    }
    CHECK(numbered && longest <= 1000, "its lines are numbered and none is over 1,000 characters");
    // the same stall, asked again: one line each, four at most
    const size_t n1 = lines.size();
    gpu_trail_dump("no frame for 20 s");
    CHECK(lines.size() == n1 + 1 && asked == 2 && lines.back().find("asked again at ") != std::string::npos &&
              lines.back().find("(no frame for 20 s): nothing was submitted and no timeline has moved since dump 1") != std::string::npos,
          "asked again in the same stall: one line that says nothing has moved since dump 1");
    gt_note_num(gt::IMPORT, 4u << 20);
    gt_submit(gt::E_READBACK, nullptr, 0, 0, 0, 0, 0, 1, 0, true);
    gpu_trail_dump("again");
    CHECK(lines.size() == n1 + 2 && asked == 2 && lines.back().find("not written again so soon after dump 1") != std::string::npos,
          "with something new within four seconds of a dump: one line, and the driver is not asked");
    for (int i = 0; i < 6; i++) gpu_trail_dump("again");
    CHECK(lines.size() == n1 + 4 && lines.back().find("(said no more until something moves)") != std::string::npos, "four such lines, then no more");
    // a wait watched: five ticks say nothing, the sixth has the trail written (4 s after the first dump: a whole one again)
    std::this_thread::sleep_for(std::chrono::milliseconds(4100));
    lines.clear();
    {
        GtWait w("submission value", 181234);
        for (int i = 0; i < 5; i++) gt_poll();
        CHECK(lines.empty(), "a wait that has seen five of the watcher's ticks is not told yet");
        gt_poll();
        CHECK(!lines.empty() && lines[0].rfind("gpu trail: d2 begins at ", 0) == 0 &&
                  lines[0].find("(a wait for submission value 181234 has lasted 5 s on thread 77)") != std::string::npos && count("gpu trail: d2 1/") == 1 &&
                  count("#10 readback CALL NOT BACK") == 1 && count("import:made 4096 KB x1*") == 1,
              "at the sixth the trail is written again, for the wait, with what was submitted since");
        const size_t n2 = lines.size();
        for (int i = 0; i < 20; i++) gt_poll();
        CHECK(lines.size() == n2, "and that wait is not told twice");
    }
    // a present's command buffer: its notes in an entry of its own, whatever recording the thread has open
    gt_begin_recording(5, &failed);
    gt::Acc* open = gt_acc;
    {
        GtScope scope;
        gt_note(gt::BLIT_PRESENT, 2, 0, 0, 2560, 1440, 1);
        const uint64_t e = scope.submit(gt::E_PRESENT, 1);
        gt_back(e, 0);
        const gt::Entry& p = gt_trail->entries[(e - 1) % gt::kEntries];
        CHECK(open && p.kind == gt::E_PRESENT && p.det_n == 1 && p.call == gt::CALL_BACK && open->ndet == 0, "a present's blit is its own entry's, not the open recording's");
    }
    CHECK(gt_acc == open, "and the thread's recording is its own again after it");
    std::printf(failed ? "FAILED: %d\n" : "PASSED: 0 failed\n", failed);
    return failed != 0;
}
