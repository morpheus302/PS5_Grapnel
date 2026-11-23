/* (ps5, lot 66) HOOKSHOT_PS5_HELPERS_SHARED: where a thread the render thread makes runs.

   On the console a new thread starts with its maker's CPUs (FreeBSD; the PC's reset to the process's affinity in
   win32_posix.c is not built there), and the render thread is pinned to CPU 12 alone (gpu/vk/vk_replay.cpp
   replay_thread_main, host->reserved_cpus). So every thread it makes on first use runs on CPU 12 too, at the same
   round-robin priority as every thread of the title: frame interpolation's display thread (3.3-4.7 % of a CPU on the crowded
   square in essai-lot65-1, which spins on SwitchToThread for the last millisecond before each of its presents), the two
   compile workers and the GPU hang watch ("sched: mkw_vk display ... CPUs 0x1000" in every log since the render thread).
   Each of them takes the render thread's only CPU from it when it runs, and the render thread is what sets the square's
   frame. With the switch on, a thread that starts on exactly the render thread's CPUs is moved to the title's others
   (CPUs 0-11), as the game's threads are; nothing else changes: a thread made anywhere else keeps what it inherited, and
   the render thread itself, made before it pins itself, is never moved.

   Kept apart from win32_posix.c (which needs the console's headers) so tests/test_ps5_helpers_shared.py can check the rule
   on the PC. */
#pragma once
#include <stdint.h>

/* The CPUs a thread that started with `inherited` goes to: the title's (`title`) less the render thread's (`replay`) when it
   inherited exactly the render thread's; 0: leave it where it is (no render thread pinned yet, a thread made elsewhere, or
   nothing would be left) */
static inline uint64_t ps5_helper_mask(uint64_t inherited, uint64_t replay, uint64_t title) {
    if (!replay || !inherited || inherited != replay) return 0;
    const uint64_t want = title & ~replay;
    return want;
}
