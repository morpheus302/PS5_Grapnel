/* (ps5) The console's side of host/posix: what win32_posix.c, hos_main.c and mkw_glue.c call where the Linux build has a
   system call or a /proc file the PS5 lacks. Implemented in host/posix/ps5_platform.c (threads, futexes, modules, paths,
   the process), host/posix/ps5_window.c (the guest window), host/posix/ps5_files.c (open files past the console's limit)
   and host/posix/ps5_broker_client.c (the storage broker), on the payload SDK fork's platform layer (the ps5platform headers).
   Built only with RX_TARGET=ps5 (nx2/build.py), where the compiler defines __PROSPERO__; HOOKSHOT_PS5_HOST_MODEL builds the
   window's part on a PC against the platform's model of the console's kernel (scripts/ps5/build-plat-probe.sh host-test).

   State (2026-11-09, branch ps5-plat): every function compiles and is linked; the platform probe title PPSA99147
   (scripts/ps5/plat-probe) runs each on the console. docs/ps5-port.md has the plan, the census its gaps. */
#ifndef HOOKSHOT_POSIX_PS5_H
#define HOOKSHOT_POSIX_PS5_H
#if defined(__PROSPERO__) || defined(HOOKSHOT_PS5_HOST_MODEL)
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the guest window: A64_HOST_BASE (0x80_0000_0000 here, nx2/toolchain_ps5.py), HOS_GUEST_SPACE (512 GiB) ----
   (host/posix/ps5_window.c) Reserved at start (one reservation, else adjacent pieces of 16 GiB at the same addresses: the
   window's addresses are the same either way; or, when its top is refused, the part from the base up, the guest's address
   space then ending there: hos_space_end) and backed with direct memory in 64 KiB units as the runtime hands memory
   out (hos_commit) or as it is first touched (the SIGSEGV handler: mkw_glue.c commit_on_touch -> hos_ps5_guest_fault).
   Backing never takes a lock a fault could interrupt, never allocates from a heap and never changes the protection of a
   unit already backed (an imported unit keeps its GPU access; only hos_ps5_guest_reclaim protects a backed unit, and never
   a pinned one). A bit per unit says what is backed, for VirtualQuery: a unit behind the shared zeros (read and never
   written, or all zeros and put back by the reclaim) reads as zero and answers "not committed". */
int hos_ps5_guest_reserve(uint64_t base, uint64_t size);   /* 1 when reserved */
int hos_ps5_in_guest(const void *p, size_t n);
/* 0, or the kernel's error (what was backed before it stays). Every unit of the range has memory of its own and is
   read-write when it returns, and stays so for two passes of the reclaim at least (a minute), and for
   HOOKSHOT_PS5_COLD_HOLD_S (60) seconds of the cold units (a unit away brought back, a clean one made writable): what a system
   call that writes into guest memory needs, as it takes no fault the handler could answer (ReadFile, nn_fs.c, the image's
   fread) */
int hos_ps5_guest_commit(void *p, size_t n);
int hos_ps5_guest_decommit(void *p, size_t n);              /* whole units given back, the partial and the pinned ones zeroed */
/* The renderer's, before it imports [p, p + n) into the GPU (gpu/vk/vk_memory.cpp import_pool): every unit of the range
   backed, and pinned: a decommit zeroes a pinned unit and keeps it (its mapping and its GPU access as they are), for good.
   Backing and pinning are made under the lock a decommit takes, so none can come between. 0, or the kernel's error when a
   unit could not be backed (the import must then not be made: the GPU would be given a range with nothing behind it).
   HOOKSHOT_PS5_GPU_PIN=0: backs and answers the same, pins nothing. Not for the fault handler (it reads the setting) */
int hos_ps5_guest_pin(void *p, size_t n);
int hos_ps5_guest_pinned(const void *p, size_t n);          /* 1 when a unit of the range is pinned */
/* The renderer's, before it uploads a texture from [p, p + n) (gpu/vk/vk_texture.cpp upload_zeros): 1 when no unit of the
   range has memory of its own (never touched, given back since, or only read: the shared zeros), so every byte of it reads
   zero and nothing has to be read to know it. Looked at under the lock a unit is given its memory under: no write had landed
   in the range when it answers 1; one that comes after is the write watch's to report. 0 for anything else: a unit backed,
   written or not, and a range outside the window */
int hos_ps5_guest_untouched(const void *p, size_t n);
/* The renderer's, for its report of what the textures' storage holds of the window (gpu/vk/vk_texture.cpp storage_report):
   how many bytes of [p, p + n) lie in units that have memory of their own now (not the reserved ones, not those behind the
   shared zeros). When asked: *pinned_bytes, the part of them in units an import pinned (never given back); *free_bytes, the
   bytes of the units that lie whole inside the range, have memory of their own and are not pinned: what a decommit of the
   range would give back. A count made while the game runs, not a snapshot: the units' bits are read as they are, without
   the lock (so asking never holds up a fault), and nothing is backed, mapped or changed. 0 for no range and for one not all
   inside the window */
uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes);
/* 1 committed, 0 only reserved; *run: the bytes from p (to the end of its run of units in the same state) */
int hos_ps5_guest_committed(const void *p, size_t *run);
/* From a SIGSEGV handler, for an address nothing backed: backs the HOOKSHOT_PS5_FAULT_KB (1024) block around it. 1 when the
   access can be retried, 0 when the fault is not the window's to answer (outside it, or a real protection fault).
   Async-signal-safe: system calls, atomics and a spinlock per GiB that nothing holds across a fault. */
int hos_ps5_guest_fault(const void *p);
/* after hookshot_env.txt is read (stage 4): HOOKSHOT_PS5_FAULT_KB taken from it; returns the block's bytes */
uint64_t hos_ps5_guest_settings(void);

/* How the window was reserved: one reservation; adjacent pieces (the same addresses); or only the pieces from the base up
   that the console granted (at least 336 GiB: the heap and the VAMM region), the guest then told a shorter address space
   (hos_space_end, HOS_SPACE_END in host/hos.h: regions are handed out down from it) */
enum { HOS_PS5_WINDOW_NONE, HOS_PS5_WINDOW_WHOLE, HOS_PS5_WINDOW_PIECES, HOS_PS5_WINDOW_PREFIX };
extern uint64_t hos_space_end;
struct hos_ps5_guest_stats {
    uint64_t base, size;
    int mode;                 /* HOS_PS5_WINDOW_* */
    int32_t whole_result;     /* the one reservation's answer (0, or the kernel's error when pieces were used) */
    unsigned pieces;          /* reservations the window is made of */
    uint64_t piece_bytes;
    uint64_t space_end;       /* the guest's address space (512 GiB, or the prefix's end) */
    uint64_t fault_bytes;     /* what one fault backs */
    uint64_t backed_bytes, peak_backed_bytes, meta_bytes;
    uint64_t commits, commit_units, commit_failures;
    int32_t last_error;
    uint64_t faults, fault_units, fault_retries, fault_refused, fault_failures, fault_ns, fault_ns_max;
    uint64_t decommit_units, rereserve_fallbacks;
    int pin_mode;             /* HOOKSHOT_PS5_GPU_PIN as read: 1 pins, 0 not, -1 no import asked yet */
    uint64_t pins;            /* hos_ps5_guest_pin calls */
    uint64_t pinned_bytes;    /* units an import was granted: never given back */
    uint64_t pin_backed_bytes;   /* of those calls' ranges, what nothing had backed before (an import's chunk past what the game uses) */
    uint64_t pin_failures;    /* calls that could not back their range (the import refused) */
    uint64_t pinned_kept_units, pinned_kept_calls;   /* decommits that met pinned units: zeroed and kept */
    int zero_mode;            /* HOOKSHOT_PS5_ZERO_READS: 1 read faults map the shared zeros, -1 off (asked off, or not possible) */
    uint64_t zero_bytes;      /* units behind the shared zeros now (no memory of their own) */
    uint64_t zero_maps, zero_upgrades, zero_failures;   /* units so mapped; given memory of their own since; refused */
    int reclaim_mode;         /* HOOKSHOT_PS5_ZERO_RECLAIM: 1 on (=1), -1 off (unset or 0), -2 turned itself off (a call refused), 0 no pass yet */
    uint64_t reclaim_passes;
    uint64_t reclaim_units, reclaim_rebacked;   /* units put back behind the shared zeros; of those, given memory again since */
    uint64_t reclaim_flaps;   /* of those, within two passes of being put back (left alone for longer each time) */
    uint64_t reclaim_written; /* units found written between the pass's first look and its lock: kept */
    uint64_t reclaim_foreign; /* units the kernel said were not plain CPU read-write: left alone */
    uint64_t reclaim_failures;
};
void hos_ps5_guest_stats(struct hos_ps5_guest_stats *stats);
/* (2026-11-10) The reclaim of zero units: one pass over the backed units; each one that is all zeros goes back behind the
   shared zeros a read fault maps (read-only, no memory of its own until something writes it again). Safe beside the game's
   threads: a unit is looked at under its GiB's lock and read-only, so a write either landed before (the unit is kept) or
   faults and waits (host/posix/ps5_window.c's header has the sequence). Never a pinned unit, nor one a commit covered in the
   last two passes (a system call may be about to write into it), nor one written again within two passes of being put back
   (left alone 8 passes, then 16 ... 256). From one thread, 30 s apart or more (host/hos_thread.c memory_contents_main): the
   hold after a commit is counted in passes. Off unless HOOKSHOT_PS5_ZERO_RECLAIM=1 (read at the first pass); off too without the
   shared zeros (HOOKSHOT_PS5_ZERO_READS=0, or refused) and without pins (HOOKSHOT_PS5_GPU_PIN=0), and for good once the
   kernel refuses one of its calls. 1 when a pass ran */
enum {
    HOS_PS5_RECLAIM_RAN = 1,
    HOS_PS5_RECLAIM_OFF = 0,         /* HOOKSHOT_PS5_ZERO_RECLAIM unset or 0 (off unless 1, since 2026-11-12) */
    HOS_PS5_RECLAIM_NO_ZEROS = -1,   /* the shared zeros are off (or the window is not made) */
    HOS_PS5_RECLAIM_NO_PINS = -2,    /* HOOKSHOT_PS5_GPU_PIN=0: the window does not know what the GPU reaches */
    HOS_PS5_RECLAIM_REFUSED = -3,    /* the kernel refused a call (error, error_step): no more passes */
    HOS_PS5_RECLAIM_BUSY = -4,       /* another thread's pass is running */
};
struct hos_ps5_guest_reclaim {
    int state;                /* HOS_PS5_RECLAIM_* */
    uint64_t pass;            /* passes made so far, this one included */
    uint64_t looked;          /* backed units met */
    uint64_t pinned, held;    /* of those: pinned; left alone after a commit or a write soon after a reclaim */
    uint64_t zeros;           /* read all zeros, and were looked at again under their lock: */
    uint64_t reclaimed;       /*   put back behind the shared zeros */
    uint64_t written;         /*   written in between: kept */
    uint64_t changed;         /*   given back, pinned or committed in between */
    uint64_t foreign;         /*   not plain CPU read-write for the kernel, for the whole unit: left alone */
    uint64_t unanswered;      /*   of those, the kernel refused the question (2026-11-12: left alone too) */
    uint64_t rebacked;        /* units earlier passes put back that got memory of their own again since the last pass */
    uint64_t ns, lock_ns_max; /* the pass; the longest a GiB's lock was held for one unit */
    uint64_t pieces;          /* (2026-11-12) the free pieces of direct memory at the memory watch's last count */
    int crowded;              /*   1: the pass stopped there, too many pieces (or a walk of them failed since the last pass) */
    uint64_t total_reclaimed, total_rebacked, total_flaps;   /* since the start */
    uint64_t zero_units;      /* units behind the shared zeros now (the read faults' too) */
    int32_t error;            /* what the kernel answered when it refused */
    int error_step;           /* 1 the read-only protection, 2 read-write again (the unit stayed read-only), 3 the zeros' mapping,
                                 4 the protection made but the unit still writable for the kernel (error: what it said) */
};
int hos_ps5_guest_reclaim(struct hos_ps5_guest_reclaim *pass);
/* (2026-11-10) what the window's backed units hold, read as they are while the game runs: MiB backed, MiB of units that are all
   zeros, MiB of units that are one 8-byte word repeated; buf: the same three by GiB of the window, "<GiB>: b/z/r; ..." */
void hos_ps5_guest_contents(char *buf, size_t cap, uint64_t *backed_mib, uint64_t *zero_mib, uint64_t *same_mib);
/* ---- (2026-11-12) cold units on the SSD (host/posix/ps5_window.c's header has the design; notes-agents/donnees-froides-ssd.md
   the measurements behind it). HOOKSHOT_PS5_COLD: unset or 0 off; "measure": units parked, "sent to the SSD" and "read back"
   without a write nor a byte given back, the SSD timed in a file of its own; 1 or on: on; any other value: off, said as not
   understood (2026-11-13: it was on). Run from one thread,
   host/hos_thread.c memory_cold_main, after hookshot_env.txt is read ---- */
enum { HOS_PS5_COLD_OFF = 0, HOS_PS5_COLD_ON = 1, HOS_PS5_COLD_MEASURE = 2 };
/* reads the settings and makes what the mode needs (address space, the measure's unit, the files emptied); says in `said`
   what it runs with, or why it is off. The mode */
int hos_ps5_cold_start(char *said, size_t cap);
int hos_ps5_cold_mode(void);
/* one step (every 100 ms): calm, the game shows frames and its window does not grow; free_mb, the free direct memory */
void hos_ps5_cold_tick(int calm, uint64_t free_mb);
/* measure: once, the SSD timed in a file of its own (its line in buf). 1 when it ran */
int hos_ps5_cold_probe(char *buf, size_t cap);
/* the line (every 30 s, and at the memory floor's stop): what is on the SSD, parked, written, read back and at what cost */
void hos_ps5_cold_line(char *buf, size_t cap);
struct hos_ps5_cold {
    int mode, stop_why, parks_stopped, writes_stopped;
    int32_t last_error;
    uint64_t parked, spilled, spilled_zero, clean;         /* units now: parked (memory held), on the SSD (of them all zeros), clean */
    uint64_t parks, park_ns, park_ns_max, park_refused;    /* parks made, their time, refused by the kernel */
    uint64_t foreign, unanswered;                          /* units the kernel did not say were the CPU's alone (no answer) */
    uint64_t unparks, unpark_age[4], unpark_ns, unpark_ns_max;   /* parked units mapped back: by how long they were parked */
    uint64_t spills, spills_written, spills_again, spills_zero;  /* to the SSD: in all, written, with their copy, all zeros */
    uint64_t raced, staged;                                /* written for nothing (back meanwhile); staged for a write */
    uint64_t written, writes, write_ns, write_ns_max, write_errors, cap_bytes, slots_used;
    uint64_t reloads, reload_ns, reload_ns_max, read_ns, read_ns_max, reload_peak, from_reserve, reserve_now;
    uint64_t sum_mismatch, probe_reads, probe_read_errors; /* measure: sums that differ "read back" (0), the handler's timed reads */
    uint64_t dirtied, stale, released;                     /* clean units written; copies forgotten; away units decommitted */
    uint64_t ticks, calm;
    uint64_t space_lost;   /* (2026-11-13) scratch or staging addresses whose reservation the kernel did not lay again (0) */
};
void hos_ps5_cold_stats(struct hos_ps5_cold *c);
/* the free direct memory as last measured, less the allocations and plus the releases since (-1: not measured yet); the free
   pieces of the last measure, and how many measures could not walk them (the largest piece stood for the free memory) */
extern int64_t hos_ps5_free_estimate;
/* (2026-11-10) the direct memory by GiB, what small and what large allocations hold of it (ps5_platform.c: direct_place) */
void hos_ps5_direct_layout(char *buf, size_t cap);
uint64_t hos_ps5_memory_pieces(uint64_t *no_walk);
/* ---- (2026-11-11) the host heap's blocks when the direct memory is in pieces (host/posix/ps5_heap_pieces.c) ----
   A block the heap's map call cannot have in one piece of direct memory is made of several pieces, mapped side by side, when
   HOOKSHOT_PS5_HEAP_PIECES_KB=<n> allows it (pieces of n KiB and more; 0, the default: refused as before). The wraps of
   ps5_platform.c route a block's start to the functions below; the memory watch trims the heap's arenas every
   HOOKSHOT_PS5_HEAP_TRIM_S seconds (0, the default: never) and says what the blocks of pieces hold */
int hos_heap_pieces_is(int64_t start);
int hos_heap_mapper_enter(void);
void hos_heap_mapper_leave(int slot);
size_t hos_heap_pieces_min(void);
int32_t hos_heap_pieces_after_refusal(int32_t refused, int64_t search_start, int64_t search_end, size_t length, size_t alignment,
                                      int32_t type, int64_t *start_out);
int32_t hos_heap_pieces_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment);
int32_t hos_heap_pieces_release(int64_t start, size_t length);
struct hos_heap_pieces_stats {
    uint64_t blocks, bytes, pieces;
    uint64_t made, given_back, refused, full, map_failed, bad, pieces_made, smallest_kib;
};
void hos_heap_pieces_stats(struct hos_heap_pieces_stats *s);
void hos_heap_arena_note(void *mspace);
unsigned hos_heap_trim(void);
unsigned hos_heap_trim_every(void);
/* what ps5_heap_pieces.c calls (ps5_platform.c's; a test's model): a piece taken and given back as any direct memory is
   (placed, counted), the kernel's map, an unmap that reserves the range again, the largest free piece in bytes */
int32_t hos_ps5_piece_take(int64_t search_end, size_t length, size_t alignment, int32_t type, int64_t *start);
int32_t hos_ps5_piece_give(int64_t start, size_t length);
int32_t hos_ps5_piece_map(void **address, size_t length, int protection, int flags, int64_t start, size_t alignment);
void hos_ps5_piece_unmap(void *address, size_t length, int reserve);
uint64_t hos_ps5_piece_largest(void);
/* (ps5_platform.c) the watch's line on the blocks of pieces ("" when there is nothing to say); the stop line for an uncaught
   std::bad_alloc (gpu/vk/vk_vma.cpp's terminate handler) */
void hos_ps5_heap_pieces_line(char *buf, size_t cap);
void hos_ps5_heap_stop_line(const char *what);
uint64_t hos_ps5_heap_trim_now(unsigned *arenas, uint64_t *after);   /* bytes the heap gave back to a trim of its arenas */

/* ---- (2026-11-15) the driver's compiles in arenas of their own (host/posix/ps5_compile_heap.c) ----
   With HOOKSHOT_PS5_COMPILE_HEAP=1, what a thread allocates inside the driver's vkCreate*Pipelines comes from a compile
   arena, trimmed when the last compile in it ends; what the compile keeps is allocated in the thread's own arena (paused);
   the segments a compiling thread maps carry 64 KiB of guard each side, so dlmalloc never joins two. The scope's calls
   (the link wraps RADV's entry points with them; a test's model calls them itself), and the guards' (ps5_platform.c's
   wraps of ps5p_heap_map and ps5p_heap_unmap) */
int hos_compile_heap_enter(void);                       /* a token for leave (0: off, nothing to leave) */
void hos_compile_heap_leave(int token);
void hos_compile_heap_pause(void);
void hos_compile_heap_resume(void);
size_t hos_compile_heap_guard_take(int *slot);          /* the guard each side for a segment mapped now (0: none) */
void *hos_compile_heap_guard_set(int slot, void *at, size_t bytes);   /* what dlmalloc is given of what the heap mapped */
int hos_compile_heap_guard_find(void *address, size_t bytes, void **mapped_at, size_t *mapped_bytes);
void hos_compile_heap_guard_forget(int slot);
struct hos_compile_heap_stats {
    int on;
    unsigned arenas;
    uint64_t scopes, stale, trims, given_bytes, guarded, guarded_made, guarded_none, routed_refused, compile_ns;
    uint64_t footprint, peak;   /* the compile arenas' bytes now, and the most after a compile */
};
void hos_compile_heap_stats(struct hos_compile_heap_stats *st);
void *hos_compile_heap_arena_now(void);                 /* the compile arena this thread allocates from now, or NULL */
const void *hos_compile_heap_allocator(const void *given);   /* the VkAllocationCallbacks a compile is given (NULL: the device's) */

/* ---- other reservations (VirtualAlloc MEM_RESERVE outside the window, the fibers' stacks): direct memory, placed, never
   the title's 448 MiB of flexible memory, which plain mmap would take ---- */
void *hos_ps5_reserve(size_t size, int commit);   /* NULL on failure; commit: backed read-write at once */
int hos_ps5_commit(void *p, size_t n);
int hos_ps5_release(void *p, size_t size);
/* a block of direct memory of its own, backed at once (NULL on failure), and its return: what is large and comes and goes
   (a movie's pictures, host/nn_video.c), which the heap would map a block for and keep. size: the same for both */
void *hos_ps5_block(size_t size);
void hos_ps5_block_free(void *p, size_t size);
/* a thread's stack: `size` reserved, its top HOOKSHOT_PS5_STACK_MB (8 MiB) backed, the rest below unbacked (a fault there
   is an overflow, until stacks grow on touch from an alternate signal stack); the base, or NULL */
void *hos_ps5_stack(size_t size);

/* ---- threads ---- */
int hos_ps5_tid(void);              /* the kernel's thread id (Linux: gettid) */
void hos_ps5_thread_started(void);  /* each thread's first call: the IEEE MXCSR (a title starts FTZ/DAZ, 0x9fe0) */
int hos_ps5_cpus(uint64_t *mask);   /* the CPUs a title's threads run on (0x1fff measured: 13); returns the count */
void hos_ps5_set_name(void *pthread, const char *name);

/* ---- scheduling (host/posix/ps5_platform.c, 2026-11-09 after the game's first run crawled): a title's threads are real-time,
   and threads of one priority are not time-sliced (EglantinePS5 headless/performance.cpp; the platform's heap blocks
   rather than spin for it). HOOKSHOT_PS5_SCHED puts each thread Hookshot starts or adopts on a policy: auto (default:
   SCHED_FIFO made SCHED_RR at the same place in its range, anything else kept), rr, other, fifo, keep ---- */
int hos_ps5_sched_apply(void);                  /* the calling thread: 1 changed, 0 left, -errno refused */
int hos_ps5_sched_set(int policy);              /* the calling thread onto policy, its priority at the same place: 0 or errno */
const char *hos_ps5_sched_mode(void);           /* HOOKSHOT_PS5_SCHED as understood */
const char *hos_ps5_policy_name(int policy);
int hos_ps5_sched_of(void *pthread, int *policy, int *priority);   /* 0 or errno (pthread_getschedparam) */
/* one line: the setting, what the first thread was and became, how many were changed or refused, the policies' ranges */
void hos_ps5_sched_note(char *buf, size_t cap);
int64_t hos_ps5_thread_cpu_ns(void *pthread);   /* the thread's CPU time, or -errno (pthread_getcpuclockid) */
int64_t hos_ps5_process_cpu_ns(void);           /* the process's CPU time, or -errno */
int hos_ps5_current_cpu(void);                  /* the CPU the calling thread runs on now (sceKernelGetCurrentCpu), or -1 */
uint64_t hos_ps5_affinity_of(void *pthread);    /* its CPU mask (scePthreadGetaffinity), 0 if refused */
/* the process's CPU sets (cpuset_getaffinity, 8 bytes as the console takes them): 0 or errno each */
int hos_ps5_process_cpuset(uint64_t *which, int *which_error, uint64_t *root, int *root_error);
/* the CPUs a thread win32_posix.c starts is put on first (0: left as it inherits them; host/hos_thread.c sets it with
   HOOKSHOT_PS5_AFFINITY=pinned, so a thread made by one on a virtual core's CPU does not stay there) */
extern uint64_t hos_ps5_new_thread_cpus;

/* ---- win32_posix.c's threads (CreateThread's and the ones it adopted: a thread that called a Win32 thread function), for
   the scheduling report (host/hos_thread.c) ---- */
typedef struct {
    void *handle;    /* the HANDLE (guest threads: HosThread's) */
    void *pthread;
    int tid;
    char name[64];
} hos_posix_thread_info;
int hos_posix_threads(hos_posix_thread_info *out, int max);   /* the live ones, up to max */

/* ---- fibers (win32_posix.c's): a switch of our own, not getcontext/swapcontext (which corrupted the heap on the console).
   hos_ps5_fiber_init lays out a new fiber's stack and returns the stack pointer to switch to; entry must never return ---- */
void *hos_ps5_fiber_init(void *stack, size_t size, void (*entry)(void *), void *arg);
void hos_ps5_fiber_switch(void **save_rsp, void *to_rsp);

/* ---- futexes on FreeBSD's _umtx_op (Linux: SYS_futex): wait while *a == v, at most rel (NULL: forever). Should a title
   be refused _umtx_op, a table of condition variables by address takes over (hos_ps5_futex_mode says which) ---- */
long hos_ps5_futex_wait(volatile uint32_t *a, uint32_t v, const struct timespec *rel);
long hos_ps5_futex_wake(volatile uint32_t *a, int n);
int hos_ps5_futex_mode(void);   /* 1 _umtx_op, 2 the condition variables (umtx refused) */
void hos_ps5_futex_force_fallback(int on);   /* (the probe's: the condition variables' path, tested on purpose) */

/* ---- (lot 62) HOS_TSC_CLOCK=1: QueryPerformanceCounter on the time stamp counter from this call on (host/posix/tsc_clock.h),
   called once when the settings are loaded (host/hos_main.c, stage 4); line: what was done, for the trail ---- */
void hos_ps5_tsc_clock_start(char *line, size_t size);

/* ---- modules: no dlopen on the console (a title cannot load a module it brought). The libraries Linux loads at run time
   are linked into eboot.bin, and these name their entry points: "hookshot_vk" (mkwvk_init) and "vulkan-1" (RADV's
   vk_icdGetInstanceProcAddr as vkGetInstanceProcAddr). Anything else: NULL, as a library that is not there. ---- */
void *hos_ps5_module(const char *name);
void *hos_ps5_symbol(void *module, const char *name);
void *hos_ps5_self(void);   /* GetModuleHandleA(NULL) */

/* ---- paths: /app0 is the title's folder, the data folder the game's (romfs, saves, logs, caches): /data/grapnel since
   lot 80 (host/posix/ps5_data_folder.h), /data/hookshot before; no getcwd ---- */
const char *hos_ps5_exe_path(void);    /* <root>/eboot.bin: what the runtime takes as its folder (the eboot is /app0's) */
/* /data/grapnel when the title sees /data (/data/hookshot when that one is used in place: its rename failed), else /app0
   (HOOKSHOT_PS5_ROOT over all) */
const char *hos_ps5_data_root(void);
/* What hos_ps5_start found of /data: "<how>: <what was done>", how one of there, taken over, in place, made (the sandbox has
   /data: ShadowMountPlus 1.7+), "elevated (Lapy), " before it (the Lapy daemon, asked for when /app0/hookshot-elevate
   exists), or why not ("none: ...; no /data: errno 2"...). */
const char *hos_ps5_data_status(void);
int hos_ps5_data_init(void);           /* (hos_ps5_start's, single-threaded: 1 when a data folder is the root) */
const char *hos_ps5_cwd(void);
int hos_ps5_chdir(const char *dir);

/* ---- win32_posix.c's file calls (host/posix/ps5_files.c, through host/posix/ps5_fdv_hooks.h): files under /data made
   0666 (folders 0777) for the broker and FTP, big writes there through the storage broker; fdv (open files past the
   console's 249 by parking them) is off: the console refuses a title dup2 over a file (HOOKSHOT_PS5_FDV=1 tries it) ---- */
struct hos_ps5_files_stats {
    unsigned live, parked, high_water;
    unsigned long parks, unparks, limit_hits, failures;
    int disabled;   /* 1: fdv could not park here (or was never started) */
    char diag[192];   /* what fdv's start found: the number holder's socket, /dev/null, dup2 over a file */
    /* writes the storage broker made for files under the data folder (win32_posix.c's and stdio's), and those it could
       not (written by the title instead) */
    unsigned long broker_writes, broker_fallbacks;
    unsigned long long broker_bytes;
    int broker_error;       /* the last write the broker refused: -errno (0: none) */
    unsigned open_files;    /* files win32_posix.c has open now */
};
void hos_ps5_files_stats(struct hos_ps5_files_stats *stats);
/* (ps5_stdio.c's writes counted with ps5_files.c's: brokered 1 = made by the broker; 0, or the broker's -errno: the title's) */
void hos_ps5_broker_count(uint64_t bytes, int brokered);

/* ---- the controllers (host/posix/hos_sdl_ps5.c): what scePad answered, for the probe and for a report ---- */
struct hos_ps5_pad_stats {
    unsigned long long reads, states, samples, not_connected, intercepted, read_errors;   /* (states: scePadReadState, when no
                                                                                               sample was new) */
    int last_error;
    unsigned handles_opened;   /* scePadOpen calls that gave a handle (one a signed-in user, unless one signs out) */
    unsigned opens, closes;    /* pad_sdl.c's: a close is a controller it saw go */
    unsigned present;          /* players whose controller is there now */
};
void hos_ps5_pad_stats(struct hos_ps5_pad_stats *stats);

/* ---- libc: what the console's lacks and the platform layer has not (yet) ---- */
struct tm;
time_t hos_ps5_timegm(struct tm *tm);   /* (nn_applet.c's _mkgmtime: no system module exports timegm) */
int hos_ps5_fnmatch_casefold(const char *pattern, const char *name);   /* (fnmatch is only libScePosixForWebKit's) */

/* ---- the eboot's first code (host/posix/ps5_early.c, from its .preinit_array entry): the stage trail and the write-watch
   table ----
   The trail: one line a step of the start, appended to <root>/hookshot_stages.txt with a write and an fsync each (never
   stdio nor the broker), so that a launch that powers the console off still says how far it got. The data folder when the
   title sees it at the first line, else /app0; hos_ps5_trail_root moves it to the data root (after stage 3), the launch's
   lines so far copied. Every line goes to klog too. hos_ps5_trail_memory: " (flexible ... ; direct ...)" for a line */
void hos_ps5_trail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* (lot 80) the folder holding the trail's file was renamed from `from` to `to` (the data folder's takeover): its path follows */
void hos_ps5_trail_renamed(const char *from, const char *to);
/* what was last written to standard error outside hos_log (an assertion, an uncaught exception, a library's message: klog
   has them, the log file not), newlines as " | ": for the crash line (host/posix/win32_posix.c) */
void hos_ps5_stderr_tail(char *out, size_t cap);
/* the thread library's objects made by name (host/posix/ps5_pthread_count.c): a line of counts and makers; and how many
   more mutexes it gives now (up to `most`, made and destroyed again) */
void hos_ps5_pthread_counts(char *buf, size_t cap);
long hos_ps5_pthread_room(long most);
/* a line to the PC named by HOOKSHOT_PS5_LOG_UDP=<IPv4>:<port>, as one datagram; nothing when it is not set (ps5_early.c) */
void hos_ps5_udp_line(const char *line, size_t n);
void hos_ps5_memory_where(char *buf, size_t cap, uint64_t used_mb);   /* the memory in use by what took it, in MiB (ps5_platform.c) */
uint64_t hos_ps5_memory_total_mb(void);   /* the title's direct memory, all of it */
/* the direct memory by the site that allocated it, and the heap's growth by who asked (host/posix/ps5_platform.c): the sites of
   rank first.. by size, as text; the address their words are read against (tools/trail-symbols.py, "acc_words") */
void hos_ps5_direct_sites(char *buf, size_t cap, unsigned first, unsigned count);
void hos_ps5_heap_sites(char *buf, size_t cap, unsigned first, unsigned count);
uintptr_t hos_ps5_sites_image(void);
int hos_ps5_memory_free(uint64_t *direct_mb, uint64_t *flexible_mb, uint64_t *largest_mb);   /* 1 when the console answered */
double hos_ps5_since_start(void);   /* seconds since the eboot's first code: the trail's clock, and the log's */
const char *hos_ps5_trail_memory(char *buf, size_t cap);
void hos_ps5_trail_root(const char *root);
const char *hos_ps5_trail_file(void);
const char *hos_ps5_early_by(void);   /* ".preinit_array" or "constructor(101)": which ran the first code */
/* a64-kit's write-watch table (a64_dirty, 128 MiB): 1 in the image's .bss (flexible memory), 2 backed with direct memory past
   the image (the link's hookshot-ps5.ld placed it there), -1 not backed (the game cannot run); text: what was done */
int hos_ps5_dirty_state(const char **text);
/* (state 3: the table reserved, a part of 256 KiB backed at its first touch) a fault's answer, and the bytes backed so far */
int hos_ps5_dirty_fault(const void *p);
uint64_t hos_ps5_dirty_bytes(uint64_t *refused);

/* ---- the process ---- */
void hos_ps5_start(void);                          /* main's first call: /data, klog, the IEEE state, the splash */
void hos_ps5_exit(int code) __attribute__((noreturn));   /* the shell's exit, never exit() */

#ifdef __cplusplus
}
#endif
#endif /* __PROSPERO__ || HOOKSHOT_PS5_HOST_MODEL */
#endif
