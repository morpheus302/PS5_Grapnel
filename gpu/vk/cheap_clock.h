// (ps5, lot 59) The render thread's clock reads that need no system call.
//
// On the console a clock_gettime is a system call: the witness thread of HOOKSHOT_PS5_THREAD_TIMES reads hos_now_ns
// (clock_gettime(CLOCK_MONOTONIC)) 225,700 times in 200 ms, 0.886 us a read (logs/udp/essai-lot57-3.txt, "cpu: calibration"),
// about forty times a TSC read. The render thread, which the crowded market keeps 85 % busy (the "cpu: busiest" lines of
// the same log), read it about 6,000 times a frame there: lot 55s's samples of "mkw_vk replay" stop 368 times of 1,775
// at its return (libkernel +0xfec), from RX_VK_SKIP_FILE's check at every draw (145), the counter reports' timestamps
// (109: host->ticks()) and the render thread's job timing (49). Two switches, off unless set, each its own console test:
//   RX_VK_CHEAP_CLOCK=1  the renderer's own timers: skip_live's 300 ms look at the skip file and the render thread's time a job
//                        read on the TSC (prof_now, the per-phase timers' clock). Nothing the game reads changes.
//   RX_VK_REPORT_TSC=1   a counter report's timestamp (what the game reads back as the GPU's time) taken from the last present's
//                        host tick plus the TSC since then, instead of a host tick read at each report: the same value to the
//                        TSC calibration's error (~1e-4 of the time since the present), never before that present's.
#pragma once
#include <cstdint>
#ifndef LONGSHOT_ENV_H
#include "compat/longshot_env.h"
#endif

namespace cheap_clock {

/// RX_VK_CHEAP_CLOCK=1: the renderer's own timers on the TSC (read once, at the first use: on the console the settings file
/// is applied after the library loads)
inline bool timers_on() {
    static const bool v = env_on("RX_VK_CHEAP_CLOCK");
    return v;
}
/// RX_VK_REPORT_TSC=1: the counter reports' timestamps from the present's anchor and the TSC (read once, at the first use)
inline bool reports_on() {
    static const bool v = env_on("RX_VK_REPORT_TSC");
    return v;
}

/// A counter report's time on the host tick clock, in ns. on: the anchor (the host tick clock's ns at the last present,
/// anchor_ns, and the TSC's then, anchor_tsc_ns) plus the TSC since, never less than the anchor; the host's clock is not
/// read. Off, or before the first present (anchor_tsc_ns 0): host_ns(), as before
template <class HostNs>
inline uint64_t report_ns(bool on, uint64_t anchor_ns, int64_t anchor_tsc_ns, int64_t now_tsc_ns, HostNs&& host_ns) {
    if (!on || !anchor_tsc_ns) return host_ns();
    const int64_t since = now_tsc_ns - anchor_tsc_ns;
    return anchor_ns + (uint64_t)(since > 0 ? since : 0);
}

}  // namespace cheap_clock
