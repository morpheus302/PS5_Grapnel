// (ps5, lot 59) tests/test_cheap_clock.py builds this: cheap_clock.h's report_ns, and skip_live as vk_draw.cpp has it, with its
// steady_clock read replaced by a counted fake (fake_steady_now) and prof_now by a fake TSC. No Vulkan, no game.
//   test_cheap_clock <skip file> off|on
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include "cheap_clock.h"

static int failures;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), failures++; \
    } while (0)

// (the fakes the cut code reads: the system clock, counted, and the TSC)
static int64_t steady_ns = 1000000000, tsc_ns = 1000000000;
static int system_clock_reads;
static std::chrono::steady_clock::time_point fake_steady_now() {
    system_clock_reads++;
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steady_ns));
}
static int64_t prof_now() { return tsc_ns; }
static void logf(const char*, ...) {}

#include "skip_live_cut.inc"

static void write_file(const char* path, const char* text) {
    FILE* f = std::fopen(path, "wb");
    if (!f) std::exit(2);
    std::fputs(text, f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const char* path = argv[1];
    const bool on = std::string(argv[2]) == "on";
    CHECK(cheap_clock::timers_on() == on);

    // ---- report_ns: the host's clock is read when off or before the first present, never else ----
    int host_reads = 0;
    const auto host = [&] { host_reads++; return (uint64_t)777000; };
    CHECK(cheap_clock::report_ns(false, 5000, 100, 400, host) == 777000 && host_reads == 1);
    CHECK(cheap_clock::report_ns(true, 5000, 0, 400, host) == 777000 && host_reads == 2);   // (no anchor yet)
    CHECK(cheap_clock::report_ns(true, 5000, 100, 400, host) == 5300 && host_reads == 2);   // (anchor + the TSC since)
    CHECK(cheap_clock::report_ns(true, 5000, 100, 100, host) == 5000 && host_reads == 2);   // (at the anchor)
    CHECK(cheap_clock::report_ns(true, 5000, 100, 40, host) == 5000 && host_reads == 2);    // (a TSC behind: never before it)
    {   // (monotone through a frame, and the same as a host clock that runs with the TSC)
        uint64_t last = 0;
        bool monotone = true, same = true;
        const uint64_t anchor = 123456789000ull;
        const int64_t anchor_tsc = 987654321;
        for (int64_t d = 0; d <= 40000000; d += 1234567) {
            const uint64_t v = cheap_clock::report_ns(true, anchor, anchor_tsc, anchor_tsc + d, host);
            monotone = monotone && v >= last, same = same && v == anchor + (uint64_t)d;
            last = v;
        }
        CHECK(monotone && same && host_reads == 2);
    }

    // ---- skip_live: one system clock read a draw off, none on; the 300 ms between looks at the file on the clock it says ----
    write_file(path, "528c39df51bae6e5\n");
    system_clock_reads = 0;
    CHECK(skip_live(0x528c39df51bae6e5ull));
    CHECK(!skip_live(0x1234ull));
    for (int i = 0; i < 1000; i++) (void)skip_live(0x1111ull + i);
    CHECK(system_clock_reads == (on ? 0 : 1002));
    // (a new list: not read before 300 ms of the clock that decides, read after)
    write_file(path, "1234\n528c39df51bae6e5\n");
    // the other clock moves 400 ms: no new look
    (on ? steady_ns : tsc_ns) += 400000000;
    CHECK(!skip_live(0x1234ull));
    // the deciding clock moves 100 ms: still the old list
    (on ? tsc_ns : steady_ns) += 100000000;
    CHECK(!skip_live(0x1234ull));
    // and 300 ms more: the new list
    (on ? tsc_ns : steady_ns) += 300000000;
    CHECK(skip_live(0x1234ull));
    CHECK(skip_live(0x528c39df51bae6e5ull));
    CHECK(!skip_live(0x99ull));
    // (the file gone: nothing left out)
    std::remove(path);
    (on ? tsc_ns : steady_ns) += 400000000;
    CHECK(!skip_live(0x1234ull) && !skip_live(0x528c39df51bae6e5ull));
    if (failures) return 1;
    std::printf("cheap clock %s: report_ns and skip_live checked (%d system clock reads)\n", on ? "on" : "off", system_clock_reads);
    return 0;
}
