/* (ps5) The guest window on the console: host/posix/ps5.h's hos_ps5_guest_*. Built with RX_TARGET=ps5, and on a PC with
   HOOKSHOT_PS5_HOST_MODEL against PS5_PayloadSDK's model of the console's kernel (platform/tests/host_kernel.c), where the
   platform probe's memory checks run before they run on the console (scripts/ps5/build-plat-probe.sh host-test).

   The window is A64_HOST_BASE (0x80_0000_0000) + [0, 512 GiB): a guest address is the base plus the address, with no lookup
   (rt/a64.h A64_PTR). It is reserved at start, before any other reservation can take its place (hos_main.c main), in one
   reservation or, when the kernel refuses one so large, in adjacent pieces of 16 GiB (the size measured to be granted at
   the hint itself, PS5_PayloadSDK platform/docs/PROBE.md "Virtual space"): the addresses are the same either way.
   HOOKSHOT_PS5_WINDOW=whole|pieces forces one way (the probe's), HOOKSHOT_PS5_WINDOW_PIECE_GB the pieces' size.

   Backing. Each 64 KiB unit (direct memory's) is its own direct-memory allocation, mapped read-write at its place over the
   reservation at its first commit and zeroed (direct memory comes back with what it held: PROBE.md). The window's
   bookkeeping is ours, not the platform's ps5_vrange_commit, because a commit also runs inside the SIGSEGV handler (first
   touch, mkw_glue.c commit_on_touch), where the platform's commit could deadlock: it takes a pthread mutex and may calloc
   to grow its table (docs/ps5-port.md, risk M5). Here, as Wine's PS5 direct views do (ps5-forever-wine
   dlls/ntdll/unix/ps5_direct.c: "a commit never allocates metadata"):
   - the state is a bit per unit (1 MiB of .bss for the window) and the unit's direct-memory start, in a table per GiB of
     the window mapped from direct memory the first time that GiB is backed (128 KiB a GiB, and 64 KiB of notes for the
     reclaim of zero units since 2026-11-10; by system calls only);
   - each GiB has a spinlock, held only across system calls, memsets of memory just mapped and (the reclaim of zero units,
     below) the read of a unit just made read-only, so nothing that holds it can fault, and a handler that waits for it
     waits for system calls to end;
   - a commit never maps or protects a unit already backed: RADV imports guest memory by granting it GPU access
     (sceKernelMprotect with the GPU bits, PS5_Mesa winsys/ps5/radv_ps5_platform.c radv_ps5_memory_grant_gpu), and a
     commit over an imported range must not take that access away. (Only the reclaim of zero units changes a backed unit's
     protection, and never a pinned unit's: below.)
   A fault backs HOOKSHOT_PS5_FAULT_KB (1024 KiB, a power of two from 64) around the address, its own unit first: 16 times
   fewer faults than a unit a fault (about 57 us each, PROBE.md) for the same commit cost (about 30 us a unit).

   Pinned units (2026-11-10, after the console itself went down twice entering the first 3D scene). An import is only that
   mprotect: RADV keeps nothing of it in the kernel, gives nothing back when the buffer is destroyed (radv_ps5_buffer_destroy:
   "the application's memory stays its own"), and the GPU reaches the unit at the CPU's address for as long as any command it
   was given names it. So the window itself remembers which units an import was granted (hos_ps5_guest_pin, a second bit per
   unit, set under the GiB's lock by the renderer before it asks for the import), and a decommit never gives a pinned unit
   back: it zeroes it and keeps it, mapped and GPU-accessible, as host/hos_mem.c does for the ranges the renderer says are
   imported (rx_guest_imported). That check alone left ways for a unit to go from under the GPU: it is made before the
   decommit and not under one lock with it (an import made in between lost its units); it no longer answers for an import
   taken out of the renderer's table to be freed later (gpu/vk/vk_memory.cpp evicted_pools, and dead_imports where no larger
   pool covers the range), though the GPU keeps its access and the entries of the guest page table are only zeroed by a
   later submission; and VirtualFree's MEM_DECOMMIT and VirtualAlloc's MEM_RESET never asked it. A unit given back and backed
   again is mapped read-write for the CPU alone: the GPU's next access to it is to memory it has no right to. Pins are never
   taken off (nothing tells the window when the last command that names a unit is done); what that keeps is what the game
   frees under ranges the renderer once imported. HOOKSHOT_PS5_GPU_PIN=0: no pins (hos_ps5_guest_pin still backs the range and
   says when it could not).

   Decommit lays a reservation over each whole unit (which replaces the mapping, as RADV's radv_ps5_memory_unmap_at does,
   with no moment where the range is free) and gives its direct memory back; the part of a unit only partly inside is
   zeroed and kept, so Horizon's 4 KiB semantics hold (the console just keeps more).

   Zero units put back (2026-11-10, hos_ps5_guest_reclaim; off unless HOOKSHOT_PS5_ZERO_RECLAIM=1, since 2026-11-12: it never
   ran on the console when it was ported onto ps5-next-19). About 400 MiB of the backed
   units held nothing but zeros through a whole session (the system region committed at start, the rest of a fault's block
   nothing wrote, heaps the game cleared). A pass, every 30 s from the thread that counts them (host/hos_thread.c
   memory_contents_main), puts each such unit back behind the shared zeros a read fault maps, where it costs nothing until
   something writes it. The game's threads run meanwhile, so for each unit that reads zero without the lock:
     1. its GiB's lock is taken, and the unit is looked at again: backed, not pinned, not held (below);
     2. its mapping is made read-only (sceKernelMprotect). From here no write reaches it: one that was on its way either
        landed before the call returned (step 3 sees it) or faults, and its handler waits for the lock (hos_ps5_guest_fault2
        takes it for a unit whose bit is set, where it used to ask the kernel for the protection at once and refuse);
     3. its 64 KiB are read, stable now. A word that is not zero: the unit was written between the first look and step 2,
        it is made read-write again and kept. All zeros:
     4. the shared zeros are mapped over it read-only (the map replaces the mapping), then its own direct memory is given
        back, its start forgotten, the zeroed bit set and the backed bit cleared;
     5. the lock is given back. A writer that waited finds the bit set (kept: the access is retried, the mapping read-write
        again) or cleared (put back: the unit gets memory of its own, zeroed, as any unit behind the shared zeros does at its
        first write, and the access is retried into it). The write that faulted had not been made, so none is lost; a
        reader never saw anything but the unit's own bytes, zeros before and after.
   That memory of its own is zeroed before it is mapped at the unit's place (back_unit, at an address kept for it): the unit
   can be read and, once mapped, written by other threads, and zeroing it in place would show them what the memory last
   held and wipe what they wrote meanwhile.
   What the pass leaves alone:
   - pinned units (the GPU reaches them at the CPU's address, and a protection change would take its access away); with
     HOOKSHOT_PS5_GPU_PIN=0 the window does not know which those are, and the pass is off;
   - a unit the kernel does not say, for the whole unit, is plain CPU read-write with no GPU access (GPU access nobody
     pinned, a grant over part of the unit, a debugging watch), or about which it does not answer (2026-11-12: a unit was
     reclaimed when the question was refused or its answer covered the first page alone; the pins are what keeps the GPU's
     units out, this is the second fence and a doubt keeps the unit);
   - a unit a guest commit covered, for the two passes that follow: a system call that writes into guest memory takes no
     fault the handler could answer (EFAULT), which is why file reads commit their buffer first (win32_posix.c ReadFile,
     nn_fs.c, the image's fread), and such a write must not find the unit read-only. A commit takes the lock, backs what
     is not backed (a unit put back among them) and holds every unit of its range; past the hold a committed unit is
     as any other (the game's heap, 3.6 GiB committed at once, has no call that needs more);
   - a unit that got memory again within two passes of being put back (it is written all the time): left alone for 8
     passes, twice as long each time that happens again, up to 256.
   The rest of a fault's block (HOOKSHOT_PS5_FAULT_KB) does not take in the units the pass put back: each gets its memory at
   its own write, or one write would bring back fifteen units of zeros. The pass is off when the shared zeros are, and
   turns itself off at the first call the kernel refuses.

   Cold units on the SSD (2026-11-12, HOOKSHOT_PS5_COLD: off unless set; "measure" or "on"). The window only grows: in
   Hyrule Field it holds 8.7 GiB of the title's 12 and the title stops at its memory floor, while most of it is files the
   game loaded and keeps (the big heap: textures, models, shaders) and does not read again (notes-agents/donnees-froides-
   ssd.md). A thread of its own (host/hos_thread.c memory_cold_main, hos_ps5_cold_tick every 100 ms) does three things, only
   while the game is calm (frames coming, the window not growing) and only in GiB HOOKSHOT_PS5_COLD_LO_GIB (256) to _HI_GIB
   (512), the big heap:
   - it parks units at a rate (HOOKSHOT_PS5_COLD_PARK_RATE, 2000 a second): a reservation replaces the unit's mapping, its
     direct memory kept as it was (as branch ps5-park measured it). The first access faults and the handler maps the same
     memory back. Never a pinned unit, one behind the shared zeros, one a commit covered in the last HOOKSHOT_PS5_COLD_HOLD_S
     (60) seconds (a system call may be about to write into it: it takes no fault), one that came back in the last
     HOOKSHOT_PS5_COLD_HOT_S (300) seconds, nor one the kernel does not say, for all of it, is the CPU's alone (no GPU bit:
     the second fence after the pins, as for the reclaim of zero units);
   - a unit parked HOOKSHOT_PS5_COLD_AFTER_S (60) seconds without coming back is cold. While the free direct memory is under
     HOOKSHOT_PS5_COLD_FREE_MB (3072), cold units go to the SSD at HOOKSHOT_PS5_COLD_WRITE_MB_S (64) at most: the unit's
     memory is mapped read-only at an address of the thread's own (it is parked: nothing can write it), its bytes are
     written to a file of HOOKSHOT_PS5_COLD_DIR (the data root), 16 units a write, and then, under the GiB's lock, if the
     unit is still parked and did not move since (gen), its direct memory is given back. A unit all zeros is not written;
   - a fault on a unit on the SSD (hos_ps5_guest_fault2, in the faulting thread's signal handler, the GiB's lock held):
     64 KiB of direct memory (else one of HOOKSHOT_PS5_COLD_RESERVE_MB kept for this), mapped at the GiB's cold scratch
     address, filled by pread from the file, its sum checked against the one taken before the write, then mapped at its
     place. A read maps it read-only and keeps its copy (clean): parked and cold again, it goes back to the SSD without a
     new write; a write (that fault, or a later one on the read-only unit) makes it read-write and forgets the copy.
   SSD wear: a unit is written once as long as it does not change; the bytes written in a session stop at
   HOOKSHOT_PS5_COLD_CAP_MB (4096), after which only units that have their copy go back; a line every 30 s says what was
   written and read back. The files are emptied at the start.
   What cannot come back ends the title, with a line in the stage trail (cold_fatal): a read refused or short, a sum that
   differs, no memory even in the reserve, a mapping refused, its GiB's scratch address lost (2026-11-13: a reservation the
   kernel did not lay again over a scratch or staging address; such an address is never mapped over again, and the parks and
   writes stop). A game that went on would run on wrong data; the save would be
   at risk. Paths that reach a unit without a fault bring it back first, under its lock: a commit (and ReadFile, WriteFile and
   the package reads, which commit their buffer; it then holds the unit), a pin (before the import's GPU grant), a partial
   decommit. A whole decommit forgets a unit on the SSD without reading it. HOOKSHOT_PS5_COLD=measure does all of it but the
   write and the giving back: what is "on the SSD" keeps its memory and its place stays a reservation, a fault on it checks
   its sum through the scratch address and times a read of the SSD probe's own file into a unit of its own
   (hos_ps5_cold_probe, which also times writes and reads of 64 KiB and 1 MiB there): what the title would have written, read
   back and paid, and nothing of the game's depends on the SSD. Pins off (HOOKSHOT_PS5_GPU_PIN=0) turn it off. */
#if defined(__PROSPERO__) || defined(HOOKSHOT_PS5_HOST_MODEL)
#include "ps5.h"

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>
#include <ps5platform/shm.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNIT_SHIFT 16
#define UNIT ((uint64_t)1 << UNIT_SHIFT)
#define WINDOW_MAX ((uint64_t)1 << 39)
#define GIB_SHIFT 30
#define GIBS (WINDOW_MAX >> GIB_SHIFT)                    /* 512 */
#define GIB_UNITS ((uint64_t)1 << (GIB_SHIFT - UNIT_SHIFT)) /* 16384 */
/* (2026-11-10) per unit, after the GiB's table of starts: what the reclaim of zero units keeps of it (hos_ps5_guest_reclaim),
   read and written under the GiB's lock (the pass reads `until` once without it, as a hint) */
struct unit_note {
    uint16_t until;      /* a backed unit: the pass that may look at it again (a commit, a write soon after a reclaim: the
                            passes before leave it alone); a unit the reclaim put behind the zeros: memory of its own before
                            that pass is "written soon" */
    uint8_t reclaimed;   /* behind the shared zeros by the reclaim's doing (not by a read fault) */
    uint8_t flaps;       /* times in a row it was given memory soon after a reclaim */
};
#define TABLE_BYTES (GIB_UNITS * sizeof(int64_t))                         /* 128 KiB a GiB */
#define META_BYTES (TABLE_BYTES + GIB_UNITS * sizeof(struct unit_note))   /* and 64 KiB of notes: 192 KiB a GiB */
_Static_assert(META_BYTES % ((uint64_t)1 << UNIT_SHIFT) == 0, "a GiB's table is whole units of direct memory");
#define PROT_RW (PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE)
#define PROT_GPU (PS5_KERNEL_PROT_GPU_READ | PS5_KERNEL_PROT_GPU_WRITE)
#define HOLD_PASSES 2      /* after a commit: the passes that leave the unit alone (a system call may be about to write it) */
#define SOON_PASSES 2      /* memory of its own within that many passes of a reclaim: the unit is written all the time */
#define QUIET_PASSES 8     /* then left alone for that many passes, twice as many each time in a row, */
#define FLAPS_MAX 6        /* up to 8 << 5 = 256 passes (about two hours) */
#define FOREIGN_PASSES 20000   /* a unit the kernel says is not plain CPU read-write: not asked again this session */

static uint64_t guest_lo, guest_hi;
static int window_mode;
static int32_t whole_result;
static unsigned window_pieces;
static uint64_t window_piece_bytes;
static uint64_t fault_bytes = (uint64_t)1 << 20;
static int64_t direct_size;
/* a bit per unit: backed */
static uint8_t backed[WINDOW_MAX >> UNIT_SHIFT >> 3];
/* a bit per unit: pinned (an import gave it GPU access: never given back), written under its GiB's lock */
static uint8_t pinned[WINDOW_MAX >> UNIT_SHIFT >> 3];
static int pin_mode = -1;   /* HOOKSHOT_PS5_GPU_PIN: 1 (default) pins, 0 not; read at the first pin */
/* (2026-11-10) a bit per unit: read-only, the one unit of zeros behind it. A read of a unit nothing ever wrote used to back it
   with 64 KiB of its own: the renderer reads a texture's storage before it draws into it (the game's 12288 x 12288 shadow
   map: 288 MiB of zeros backed to be read once), and the area a save loads stopped at the memory floor. Now a read fault maps
   the shared zeros, and the first write (a second fault) or a commit or a pin gives the unit memory of its own.
   HOOKSHOT_PS5_ZERO_READS=0: as before. Off by itself when the zeros cannot be mapped a second time. */
static uint8_t zeroed[WINDOW_MAX >> UNIT_SHIFT >> 3];
static int zero_mode;          /* 0 not asked yet (off), 1 on, -1 off */
static int64_t zero_start = -1;
/* per GiB: the units' direct-memory starts plus one (0: none), in direct memory mapped at meta_base + GiB * META_BYTES */
static int64_t *meta[GIBS];
static uint8_t *meta_base;
/* a unit of address space per GiB, used under the GiB's lock: where the new memory of a unit behind the shared zeros is
   zeroed before it is mapped over them (back_unit) */
static uint8_t *scratch_base;
static volatile int gib_lock[GIBS];
/* the reclaim of zero units (the file's header): HOOKSHOT_PS5_ZERO_RECLAIM as read at the first pass (0 not asked yet, 1 on,
   -1 off (unset or 0), -2 turned itself off: the kernel refused a call), the passes started (a unit's notes count in them), and one
   pass at a time */
static int reclaim_mode;
static uint32_t reclaim_pass;
static int reclaim_busy;
static int32_t reclaim_error;
static int reclaim_error_step;
#ifdef HOOKSHOT_PS5_HOST_MODEL
/* (the host test's: called with a unit's guest address at step 0, the unit read zero and its lock not taken yet, and at step
   1, the unit read-only and the lock held, so that a write can be placed at each moment and not left to chance) */
void (*hos_ps5_reclaim_hook)(int step, uint64_t guest);
#define RECLAIM_HOOK(step, u) do { if (hos_ps5_reclaim_hook) hos_ps5_reclaim_hook((step), (u) << UNIT_SHIFT); } while (0)
#else
#define RECLAIM_HOOK(step, u) ((void)0)
#endif

/* ---- (2026-11-12) cold units on the SSD (the file's header): per unit of a GiB the cold thread works in, a note in direct
   memory mapped at cold_base + GiB * COLD_BYTES the first time (none at all while HOOKSHOT_PS5_COLD is off: a GiB with no
   notes behaves as before, one pointer read). Written under the GiB's lock; read without it as a hint ---- */
enum {
    C_PARKED = 1,    /* its place a reservation, its direct memory held (its start in the GiB's table) */
    C_SPILLED = 2,   /* its place a reservation, its bytes on the SSD (slot) and its memory given back (measure: held) */
    C_CLEAN = 4,     /* backed and mapped read-only: its copy (slot, or C_ZERO) is its bytes; a write makes it read-write */
    C_FOREIGN = 8,   /* the kernel did not say the unit is the CPU's alone (GPU access, part of it, no answer): never parked */
    C_ZERO = 16,     /* its copy is "all zeros" (nothing written for it) */
    C_STAGED = 32,   /* the writer has its memory mapped at its staging address: only the writer gives that memory back */
    C_DEAD = 64,     /* staged, and decommitted meanwhile: the writer gives the staged memory back */
};
struct cold_note {
    uint8_t state;   /* C_* */
    uint8_t gen;     /* one more each time the unit is mapped back or given back: the writer's check that it did not move */
    uint16_t hold;   /* the cold clock in seconds (mod 65536) before which the unit is not parked */
    uint32_t slot;   /* its copy on the SSD: slot index + 1 (0: none) */
    uint32_t when;   /* parked at: the cold clock in ms (mod 2^32) */
    uint32_t sum;    /* the sum of its 64 KiB as the copy holds them (cold_sum) */
};
_Static_assert(sizeof(struct cold_note) == 16, "a cold note is 16 bytes");
#define COLD_BYTES (GIB_UNITS * sizeof(struct cold_note))   /* 256 KiB a GiB */
_Static_assert(COLD_BYTES % ((uint64_t)1 << UNIT_SHIFT) == 0, "a GiB's cold notes are whole units of direct memory");
#define COLD_BATCH 16          /* units a write (1 MiB), and the writer's staging units */
#define COLD_SEGS 64           /* files on the SSD at most (HOOKSHOT_PS5_COLD_SEG_MB each) */
#define COLD_RESERVE_MAX 1024  /* units of the reserve at most (64 MiB) */
static struct cold_note *cold_notes[GIBS];
static uint8_t *cold_base;      /* the notes' address range (GIBS * COLD_BYTES, reserved at the start) */
static uint8_t *cold_scratch;   /* a unit of address space per GiB: where a unit coming back is filled (its GiB's lock held) */
static uint8_t *cold_staging;   /* COLD_BATCH units of address space: where the writer maps the units it writes */
static uint8_t *cold_sink;      /* measure: a unit of memory the fault handler's timed reads of the probe's file land in */
static int cold_mode;           /* 0 not started, HOS_PS5_COLD_ON, HOS_PS5_COLD_MEASURE; -1 off */
static int cold_failed;         /* cold_fatal under way */
static int cold_fd[COLD_SEGS];  /* the files' descriptors + 1 (0: not open) */
static int cold_probe_fd = -1;  /* measure: the probe's file, for the fault handler's timed reads */
static uint64_t cold_probe_slots;
static uint64_t cold_clock0;    /* the cold clock's 0 (now_ns at the start) */
static struct {
    uint64_t lo, hi;                         /* units [lo, hi): HOOKSHOT_PS5_COLD_LO_GIB, _HI_GIB */
    uint64_t lo_gib, hi_gib;
    uint64_t park_rate, after_ms, hot_s, hold_s, free_mb, write_bps, cap_bytes, reserve_units, seg_slots, seg_mb;
    char dir[160];
    /* the cold thread's own */
    uint64_t park_cursor, spill_cursor, last_ns, park_credit, write_credit, slots_used;
    int parks_stopped, writes_stopped, stop_why;   /* stop_why: COLD_STOP_* */
    uint64_t pieces, crowded, no_walk_seen;        /* the free pieces at the last step; steps that sent nothing for them */
} ck;
/* (the free pieces of direct memory: each unit given back is one more at most, and the memory watch walks them, giving up
   past 8192 questions, the largest piece then standing for the free memory and the title ending early at its floor; the
   reclaim of zero units keeps under the same count) */
#define COLD_PIECES_MAX 3000
enum { COLD_STOP_NONE, COLD_STOP_PARK_REFUSED, COLD_STOP_OPEN, COLD_STOP_WRITE, COLD_STOP_CAP, COLD_STOP_PINS, COLD_STOP_SPACE };
static int64_t cold_reserve[COLD_RESERVE_MAX];
static unsigned cold_reserve_n;
static int cold_reserve_lock;
static uint64_t cold_second;    /* the units back from the SSD in the current second of the cold clock: (second << 24) | count */
/* (2026-11-13, the review) a scratch or staging address whose reservation the kernel did not lay again (lay_reservation refused:
   it may have unmapped the range and not reserved it, and another mapping may be given that place): never mapped over again,
   as back_unit forgets its own scratch addresses then. The count; a GiB's scratch lost, written under its lock (a unit of that
   GiB on the SSD can no longer be checked before it is at its place: the title ends if one is touched); the cold thread stops
   its parks and writes at its next step */
static uint32_t cold_space_lost;
static uint8_t cold_scratch_lost[GIBS];
static void space_lost(void) { __atomic_add_fetch(&cold_space_lost, 1, __ATOMIC_RELEASE); }
#ifdef HOOKSHOT_PS5_HOST_MODEL
/* (the host test's: what cold_fatal does there instead of ending the process; and a call at the writer's steps, step 0 once
   a batch is staged and nothing written, 1 once it is written and nothing given back, so that a fault, a decommit or a
   commit can be placed between them; units: the batch's units, where they are) */
void (*hos_ps5_cold_fatal_hook)(const char *what, uint64_t guest, int32_t error);
void (*hos_ps5_cold_writer_hook)(int step, const uint64_t *units, unsigned n);
#define WRITER_HOOK(step, units, n) do { if (hos_ps5_cold_writer_hook) hos_ps5_cold_writer_hook((step), (units), (n)); } while (0)
#else
#define WRITER_HOOK(step, units, n) ((void)0)
#endif
/* (ps5_early.c's and ps5_platform.c's, absent from the kernel model's link) */
void hos_ps5_trail(const char *fmt, ...) __attribute__((weak));
const char *hos_ps5_data_root(void) __attribute__((weak));

static struct {
    uint64_t backed_units, peak_units, meta_bytes;
    uint64_t commits, commit_units, commit_failures;
    int32_t last_error;
    uint64_t faults, fault_units, fault_retries, fault_refused, fault_failures, fault_ns, fault_ns_max;
    uint64_t decommit_units, rereserve_fallbacks;
    uint64_t pins, pinned_units, pin_backed_units, pin_failures, pinned_kept_units, pinned_kept_calls;
    uint64_t zero_units, zero_maps, zero_upgrades, zero_failures;
    uint64_t reclaim_passes, reclaim_units, reclaim_rebacked, reclaim_flaps, reclaim_written, reclaim_foreign, reclaim_failures;
    /* cold units (the struct hos_ps5_cold in ps5.h says what each is) */
    uint64_t cold_parked, cold_spilled, cold_clean, cold_spilled_zero;
    uint64_t cold_parks, cold_park_ns, cold_park_ns_max, cold_park_refused, cold_foreign, cold_unanswered;
    uint64_t cold_unparks, cold_unpark_age[4], cold_unpark_ns, cold_unpark_ns_max;
    uint64_t cold_spills, cold_spills_written, cold_spills_again, cold_spills_zero, cold_raced, cold_staged;
    uint64_t cold_written, cold_writes, cold_write_ns, cold_write_ns_max, cold_write_errors;
    uint64_t cold_reloads, cold_reload_ns, cold_reload_ns_max, cold_read_ns, cold_read_ns_max, cold_reload_peak, cold_from_reserve;
    uint64_t cold_sum_mismatch, cold_probe_reads, cold_probe_read_errors, cold_dirtied, cold_stale, cold_released, cold_ticks, cold_calm;
} st;

#define ADD(field, v) __atomic_add_fetch(&st.field, (v), __ATOMIC_RELAXED)

/* ---- small things that are safe in a signal handler: no locks, no allocation, no stdio ---- */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static char *put_str(char *at, char *end, const char *s) {
    while (*s && at < end) *at++ = *s++;
    return at;
}
static char *put_hex(char *at, char *end, uint64_t v) {
    char digits[16];
    int n = 0;
    do digits[n++] = "0123456789abcdef"[v & 15], v >>= 4; while (v && n < 16);
    at = put_str(at, end, "0x");
    while (n && at < end) *at++ = digits[--n];
    return at;
}
static char *put_dec(char *at, char *end, uint64_t v) {
    char digits[20];
    int n = 0;
    do digits[n++] = (char)('0' + v % 10), v /= 10; while (v && n < 20);
    while (n && at < end) *at++ = digits[--n];
    return at;
}
static void say_fault(const char *what, uint64_t guest, uint64_t units, int32_t error, uint64_t ns) {
    char line[160], *at = line, *const end = line + sizeof line - 2;
    at = put_str(at, end, "[hookshot] ps5: guest ");
    at = put_hex(at, end, guest);
    at = put_str(at, end, what);
    at = put_dec(at, end, units);
    at = put_str(at, end, " units in ");
    at = put_dec(at, end, ns / 1000);
    at = put_str(at, end, " us");
    if (error) at = put_str(at, end, ", error "), at = put_hex(at, end, (uint32_t)error);
    *at++ = '\n';
    *at = 0;
    sceKernelDebugOutText(0, line);
}

static void lock_gib(uint64_t g) {
    unsigned spins = 0;
    while (__atomic_exchange_n(&gib_lock[g], 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&gib_lock[g], __ATOMIC_RELAXED)) {
            if (++spins < 128) __builtin_ia32_pause();
            else sched_yield();
        }
}
static void unlock_gib(uint64_t g) { __atomic_store_n(&gib_lock[g], 0, __ATOMIC_RELEASE); }

static int bit(uint64_t u) { return __atomic_load_n(&backed[u >> 3], __ATOMIC_ACQUIRE) >> (u & 7) & 1; }
static void set_bit(uint64_t u, int on) {
    if (on) __atomic_fetch_or(&backed[u >> 3], (uint8_t)(1u << (u & 7)), __ATOMIC_RELEASE);
    else __atomic_fetch_and(&backed[u >> 3], (uint8_t)~(1u << (u & 7)), __ATOMIC_RELEASE);
}

static int pin_bit(uint64_t u) { return __atomic_load_n(&pinned[u >> 3], __ATOMIC_ACQUIRE) >> (u & 7) & 1; }
static int zero_bit(uint64_t u) { return __atomic_load_n(&zeroed[u >> 3], __ATOMIC_ACQUIRE) >> (u & 7) & 1; }
static void set_zero_bit(uint64_t u, int on) {
    if (on) __atomic_fetch_or(&zeroed[u >> 3], (uint8_t)(1u << (u & 7)), __ATOMIC_RELEASE);
    else __atomic_fetch_and(&zeroed[u >> 3], (uint8_t)~(1u << (u & 7)), __ATOMIC_RELEASE);
}

static int32_t allocate(size_t bytes, int64_t *start) {
    return sceKernelAllocateDirectMemory(0, direct_size, bytes, UNIT, PS5_KERNEL_DIRECT_TYPE_CPU, start);
}

/* the GiB's table (its lock held): mapped at its first use */
static int32_t meta_of(uint64_t g, int64_t **out) {
    if (meta[g]) { *out = meta[g]; return 0; }
    int64_t start = -1;
    int32_t r = allocate(META_BYTES, &start);
    if (r) return r;
    void *at = meta_base + g * META_BYTES, *placed = at;
    r = sceKernelMapDirectMemory(&placed, META_BYTES, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, META_BYTES), r = PS5_SHM_NO_PLACE;
    if (r) { sceKernelReleaseDirectMemory(start, META_BYTES); return r; }
    memset(at, 0, META_BYTES);
    __atomic_store_n(&meta[g], (int64_t *)at, __ATOMIC_RELEASE);
    ADD(meta_bytes, META_BYTES);
    *out = (int64_t *)at;
    return 0;
}

/* unit u's notes (its GiB's table mapped: a unit of that GiB was backed once) */
static struct unit_note *note_of(uint64_t u) {
    return (struct unit_note *)((uint8_t *)meta[u / GIB_UNITS] + TABLE_BYTES) + u % GIB_UNITS;
}
/* pass numbers are the low 16 bits of a count that goes on: `pass` comes before `until` (they are never 32,768 passes, eleven
   days, apart) */
static int before(uint16_t pass, uint16_t until) { return (int16_t)(uint16_t)(until - pass) > 0; }

/* ---- (2026-11-12) cold units: what the window's paths ask, and bringing a unit back (the file's header) ---- */
static void *place(uint64_t u) { return (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT)); }
/* unit u's cold note, or NULL when its GiB has none (the cold thread never worked there, or it is off) */
static struct cold_note *cnote(uint64_t u) {
    struct cold_note *const t = __atomic_load_n(&cold_notes[u / GIB_UNITS], __ATOMIC_ACQUIRE);
    return t ? t + u % GIB_UNITS : NULL;
}
static unsigned cstate(const struct cold_note *n) { return n ? __atomic_load_n(&n->state, __ATOMIC_ACQUIRE) : 0; }
static void cset(struct cold_note *n, unsigned s) { __atomic_store_n(&n->state, (uint8_t)s, __ATOMIC_RELEASE); }
/* away: its place a reservation, its bytes held (parked) or on the SSD; a fault, a commit or a pin brings it back */
static unsigned cold_away(uint64_t u) { return cstate(cnote(u)) & (C_PARKED | C_SPILLED); }
/* a unit with data: backed, or away */
static int has_data(uint64_t u) { return bit(u) || cold_away(u); }
/* a unit with its direct memory held: backed, parked, or (measure) "on the SSD" */
static int holds_memory(uint64_t u) {
    const unsigned s = cold_away(u);
    return bit(u) || (s & C_PARKED) || ((s & C_SPILLED) && cold_mode == HOS_PS5_COLD_MEASURE);
}
static int has_copy(const struct cold_note *n) { return n->slot || (n->state & C_ZERO); }
static uint64_t cold_ms(void) { return (now_ns() - cold_clock0) / 1000000; }
static uint16_t cold_s16(void) { return (uint16_t)(cold_ms() / 1000); }
static void max_of(uint64_t *at, uint64_t v) {
    uint64_t m = __atomic_load_n(at, __ATOMIC_RELAXED);
    while (v > m && !__atomic_compare_exchange_n(at, &m, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/* the copy forgotten: the unit is (about to be) writable, or gone */
static void drop_copy(struct cold_note *n) {
    if (n->slot) ADD(cold_stale, 1);
    n->slot = 0;
    cset(n, n->state & ~(C_ZERO | C_CLEAN));
}
/* the sum of 64 KiB: four lanes of FNV-1a over 64-bit words, folded (a torn, short or wrong read changes it) */
static uint32_t cold_sum(const void *p) {
    const uint64_t *w = (const uint64_t *)p, k = 0x100000001b3ull;
    uint64_t a = 0xcbf29ce484222325ull, b = a ^ 1, c = a ^ 2, d = a ^ 3;
    for (size_t i = 0; i < UNIT / 8; i += 4) a = (a ^ w[i]) * k, b = (b ^ w[i + 1]) * k, c = (c ^ w[i + 2]) * k, d = (d ^ w[i + 3]) * k;
    const uint64_t h = (a * 31 + b) * 31 * 31 + c * 31 + d;
    return (uint32_t)(h ^ h >> 32);
}
static int all_zero(const void *at);
/* a reservation laid over [at, at + bytes) again, as unback_unit lays one (the address stays the window's or the thread's) */
static int32_t lay_reservation(void *at, size_t bytes) {
    void *placed = at;
    int32_t r = sceKernelReserveVirtualRange(&placed, bytes, PS5_KERNEL_MAP_FIXED, UNIT);
    if (r == 0 && placed != at) sceKernelMunmap(placed, bytes), r = PS5_SHM_NO_PLACE;
    if (r) {   /* (unmapped, then reserved again) */
        ADD(rereserve_fallbacks, 1);
        placed = at;
        r = sceKernelMunmap(at, bytes);
        if (r == 0) r = sceKernelReserveVirtualRange(&placed, bytes, PS5_KERNEL_MAP_FIXED, UNIT);
        if (r == 0 && placed != at) sceKernelMunmap(placed, bytes), r = PS5_SHM_NO_PLACE;
    }
    return r;
}
static int32_t map_fixed(void *at, int prot, int64_t start) {
    void *placed = at;
    int32_t r = sceKernelMapDirectMemory(&placed, UNIT, prot, PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
    return r;
}

/* A unit that cannot come back as it was: the title ends here, with why in the stage trail, rather than run on (the faulting
   thread waits on nothing: the GiB's lock may be held, and nothing on this way touches guest memory). Signal-safe as the
   crash path's own trail lines are (host/posix/win32_posix.c ps5_trail_signal) */
static void cold_fatal(uint64_t u, const char *what, int32_t error) __attribute__((noreturn));
static void cold_fatal(uint64_t u, const char *what, int32_t error) {
    __atomic_store_n(&cold_failed, 1, __ATOMIC_RELEASE);
    say_fault(": cold unit, NOT BROUGHT BACK, the title ends: ", u << UNIT_SHIFT, 1, error, 0);
    if (hos_ps5_trail)
        hos_ps5_trail("memory: STOPPING THE TITLE: guest %#llx, a unit set aside on the SSD: %s (error %#x); a game that went on would run "
                      "on wrong data (HOOKSHOT_PS5_COLD=%s; %llu MiB on the SSD, %llu MiB read back so far)",
                      (unsigned long long)(u << UNIT_SHIFT), what, (unsigned)error,
                      cold_mode == HOS_PS5_COLD_MEASURE ? "measure" : "on",
                      (unsigned long long)(__atomic_load_n(&st.cold_spilled, __ATOMIC_RELAXED) >> 4),
                      (unsigned long long)(__atomic_load_n(&st.cold_reloads, __ATOMIC_RELAXED) >> 4));
#ifdef HOOKSHOT_PS5_HOST_MODEL
    if (hos_ps5_cold_fatal_hook) hos_ps5_cold_fatal_hook(what, u << UNIT_SHIFT, error);
    printf("cold_fatal: %s at %#llx (error %#x)\n", what, (unsigned long long)(u << UNIT_SHIFT), (unsigned)error);
    fflush(stdout);
    _exit(77);
#else
    abort();
#endif
}

/* a unit of the reserve (the fault handler's, when the kernel refuses 64 KiB): 0, or nothing left */
static void reserve_take(void) {
    while (__atomic_exchange_n(&cold_reserve_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}
static void reserve_give(void) { __atomic_store_n(&cold_reserve_lock, 0, __ATOMIC_RELEASE); }
static int reserve_pop(int64_t *start) {
    int r = -1;
    reserve_take();
    if (cold_reserve_n) *start = cold_reserve[--cold_reserve_n], r = 0;
    reserve_give();
    return r;
}

/* the slot's 64 KiB into `to` (the fault handler's too: a system call; errno as it was): 0, or the error (EIO: short) */
static int32_t cold_read(uint32_t slot, void *to, uint64_t *ns) {
    const int saved = errno;
    const uint64_t i = slot - 1, seg = i / ck.seg_slots, t0 = now_ns();
    const int fd = seg < COLD_SEGS ? __atomic_load_n(&cold_fd[seg], __ATOMIC_ACQUIRE) - 1 : -1;
    const off_t off = (off_t)((i % ck.seg_slots) << UNIT_SHIFT);
    int32_t r = fd < 0 ? EBADF : 0;
    for (size_t got = 0; r == 0 && got < UNIT;) {
        const ssize_t n = pread(fd, (uint8_t *)to + got, UNIT - got, off + (off_t)got);
        if (n > 0) got += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else r = n == 0 ? EIO : errno;
    }
    *ns = now_ns() - t0;
    errno = saved;
    return r;
}

/* (measure) what a unit "on the SSD" would have brought back: its own memory read through the scratch address, its sum
   compared with the one taken when it was "written" (they must agree: nothing could write it), and a read of the probe's file
   timed into a unit of its own, the read the SSD would have made here. Nothing of the unit depends on it. Its lock held */
static void cold_measure_back(uint64_t u, const struct cold_note *n, int64_t start, uint64_t *read_ns) {
    const uint64_t g = u / GIB_UNITS;
    void *const scratch = cold_scratch + g * UNIT;
    /* (2026-11-13: a scratch address lost is not mapped over again; the sum not checked is said as one that differs, the
       measurement's own stop) */
    if (cold_scratch_lost[g]) ADD(cold_sum_mismatch, 1);
    else if (map_fixed(scratch, PS5_KERNEL_PROT_CPU_READ, start) == 0) {
        if (cold_sum(scratch) != n->sum) ADD(cold_sum_mismatch, 1);
        if (lay_reservation(scratch, UNIT)) cold_scratch_lost[g] = 1, space_lost();
    } else ADD(cold_sum_mismatch, 1);
    const int fd = __atomic_load_n(&cold_probe_fd, __ATOMIC_ACQUIRE);
    if (fd >= 0 && cold_sink && cold_probe_slots) {
        const int saved = errno;
        const uint64_t t0 = now_ns();
        const ssize_t got = pread(fd, cold_sink, UNIT, (off_t)(((u * 7) % cold_probe_slots) << UNIT_SHIFT));
        *read_ns = now_ns() - t0;
        if (got != (ssize_t)UNIT) ADD(cold_probe_read_errors, 1);
        ADD(cold_probe_reads, 1);
        errno = saved;
    }
}

/* Unit u, away (parked or on the SSD), its GiB's lock held: its own bytes back at its place. Read-only, its copy kept, when
   write is 0 and it has a copy; else read-write and the copy forgotten. The backed bit is set before the cold state is
   cleared, so that a reader without the lock sees a unit with data throughout. From a fault handler too: no lock but the
   GiB's (and the reserve's, held for a few loads), nothing allocated from a heap, nothing logged. 0; what cannot come back
   ends the title (cold_fatal) */
static int32_t cold_back(uint64_t u, struct cold_note *n, int write) {
    const uint64_t t0 = now_ns();
    const unsigned s = n->state;
    const int keep = !write && has_copy(n);
    const int prot = keep ? PS5_KERNEL_PROT_CPU_READ : PROT_RW;
    int64_t *const table = meta[u / GIB_UNITS];
    const int from_ssd = (s & C_SPILLED) != 0;
    uint64_t read_ns = 0;
    int32_t r;
    if (!from_ssd || cold_mode == HOS_PS5_COLD_MEASURE) {
        const int64_t start = table[u % GIB_UNITS] - 1;   /* (its own memory, held while it was away) */
        if (from_ssd) cold_measure_back(u, n, start, &read_ns);
        /* (refused: the unit cannot be reached again, and the access that wants it would end the game without a word) */
        if ((r = map_fixed(place(u), prot, start)) != 0) cold_fatal(u, "its own memory could not be mapped back at its place", r);
    } else {
        /* on the SSD: 64 KiB of its own again (or of the reserve), filled at the GiB's scratch address, then mapped. (2026-11-13:
           that address lost, its bytes have nowhere to be checked before they are at their place, where another thread could
           read them: the title ends rather than map over an address that may be someone else's now) */
        if (cold_scratch_lost[u / GIB_UNITS]) cold_fatal(u, "its GiB's scratch address was lost (the kernel did not lay its reservation again)", 0);
        int64_t start = -1;
        r = allocate(UNIT, &start);
        if (r) {
            if (reserve_pop(&start)) cold_fatal(u, "no direct memory to bring it back into, the reserve spent", r);
            ADD(cold_from_reserve, 1);
        }
        void *const scratch = cold_scratch + (u / GIB_UNITS) * UNIT;
        if ((r = map_fixed(scratch, PROT_RW, start)) != 0) cold_fatal(u, "its scratch address could not be mapped", r);
        if (s & C_ZERO) memset(scratch, 0, UNIT);
        else if ((r = cold_read(n->slot, scratch, &read_ns)) != 0) cold_fatal(u, "its bytes could not be read back from the SSD", r);
        if (cold_sum(scratch) != n->sum) cold_fatal(u, "the bytes read back from the SSD are not the ones written (sum)", 0);
        if (lay_reservation(scratch, UNIT)) cold_scratch_lost[u / GIB_UNITS] = 1, space_lost();   /* (this unit is right: it goes on) */
        if ((r = map_fixed(place(u), prot, start)) != 0) cold_fatal(u, "its memory could not be mapped at its place", r);
        table[u % GIB_UNITS] = start + 1;
        const uint64_t now = ADD(backed_units, 1);
        max_of(&st.peak_units, now);
    }
    set_bit(u, 1);
    unsigned next = s & ~(C_PARKED | C_SPILLED);
    if (keep) next |= C_CLEAN, ADD(cold_clean, 1);
    cset(n, next);
    if (!keep) drop_copy(n);
    n->gen++;
    if (s & C_PARKED) __atomic_sub_fetch(&st.cold_parked, 1, __ATOMIC_RELAXED);
    if (s & C_SPILLED) {
        __atomic_sub_fetch(&st.cold_spilled, 1, __ATOMIC_RELAXED);
        if (s & C_ZERO) __atomic_sub_fetch(&st.cold_spilled_zero, 1, __ATOMIC_RELAXED);
    }
    /* back: warm, not parked again for a while (twice as long when the SSD had to be read) */
    const uint64_t now_ms = cold_ms(), age = (uint32_t)((uint32_t)now_ms - n->when);
    n->hold = (uint16_t)(now_ms / 1000 + (from_ssd ? 2 : 1) * ck.hot_s);
    const uint64_t ns = now_ns() - t0;
    if (from_ssd) {
        ADD(cold_reloads, 1);
        ADD(cold_reload_ns, ns);
        max_of(&st.cold_reload_ns_max, ns);
        ADD(cold_read_ns, read_ns);
        max_of(&st.cold_read_ns_max, read_ns);
        const uint64_t sec = now_ms / 1000;
        uint64_t old = __atomic_load_n(&cold_second, __ATOMIC_RELAXED), nx;
        do nx = (old >> 24) == sec ? old + 1 : (sec << 24 | 1);
        while (!__atomic_compare_exchange_n(&cold_second, &old, nx, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
        max_of(&st.cold_reload_peak, nx & 0xffffff);
    } else {
        ADD(cold_unparks, 1);
        ADD(cold_unpark_ns, ns);
        max_of(&st.cold_unpark_ns_max, ns);
        ADD(cold_unpark_age[age < 1000 ? 0 : age < 10000 ? 1 : age < 60000 ? 2 : 3], 1);
    }
    return 0;
}
/* a clean unit (backed, read-only, its copy its bytes) made read-write and its copy forgotten, its GiB's lock held: the
   protection, or should the kernel refuse it, its own memory mapped again read-write. 0, or the kernel's error (the unit
   stays read-only: a write into it would end the game, so the callers end the title with a line instead) */
static int32_t make_writable(uint64_t u, struct cold_note *n) {
    int32_t r = sceKernelMprotect(place(u), UNIT, PROT_RW);
    if (r) r = map_fixed(place(u), PROT_RW, meta[u / GIB_UNITS][u % GIB_UNITS] - 1);
    if (r) return r;
    drop_copy(n);
    __atomic_sub_fetch(&st.cold_clean, 1, __ATOMIC_RELAXED);
    ADD(cold_dirtied, 1);
    return 0;
}
/* whatever the cold state of unit u (its GiB's lock held, the unit backed or away), a unit with its bytes, read-write, its
   copy forgotten: what a commit, a pin and a partial decommit need before a system call, the GPU or a memset reaches it */
static int32_t cold_writable(uint64_t u) {
    struct cold_note *const n = cnote(u);
    const unsigned s = cstate(n);
    if (s & (C_PARKED | C_SPILLED)) return cold_back(u, n, 1);
    if ((s & C_CLEAN) && bit(u)) {
        const int32_t r = make_writable(u, n);
        if (r) cold_fatal(u, "a read-only unit could not be made writable", r);
    }
    return 0;
}

/* back unit u (its GiB's lock held) */
static int32_t back_unit(uint64_t u) {
    if (bit(u)) return 0;
    /* (2026-11-12: a unit away gets its own bytes back, never fresh memory) */
    if (cold_away(u)) return cold_back(u, cnote(u), 1);
    int64_t *table;
    int32_t r = meta_of(u / GIB_UNITS, &table);
    if (r) return r;
    int64_t start = -1;
    r = allocate(UNIT, &start);
    if (r) return r;
    void *at = (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT)), *placed = at;
    uint8_t *const scratches = __atomic_load_n(&scratch_base, __ATOMIC_ACQUIRE);
    const int readable = zero_bit(u) && scratches;
    if (readable) {
        /* (2026-11-10) A unit behind the shared zeros can be read, and is: mapped at its place and zeroed there, its new
           memory showed what it last held to a thread reading the unit at that moment, and the memset wiped a write that
           landed after the map (the reclaim makes this the path of every unit written again: four threads writing beside
           it lost bytes that way in the host test). So its memory is zeroed at an address of this GiB's own first, as the
           unit of zeros itself is made, and mapped over the zeros after: the unit reads zeros before and after, and takes
           writes only once it does. (A unit nothing touched cannot be read before its map: an access faults and waits for
           this lock. It still can during its memset, as it always could.) */
        void *const scratch = scratches + (u / GIB_UNITS) * UNIT;
        placed = scratch;
        r = sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
        if (r == 0 && placed != scratch) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
        if (r) { sceKernelReleaseDirectMemory(start, UNIT); return r; }
        memset(scratch, 0, UNIT);
        placed = scratch;   /* (a reservation laid over it again, as unback_unit lays one: the address stays this GiB's) */
        int32_t gone = sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
        if (gone == 0 && placed != scratch) sceKernelMunmap(placed, UNIT), gone = PS5_SHM_NO_PLACE;
        if (gone) {   /* (unmapped, then reserved again, as unback_unit falls back) */
            ADD(rereserve_fallbacks, 1);
            placed = scratch;
            gone = sceKernelMunmap(scratch, UNIT);
            if (gone == 0) gone = sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
            if (gone == 0 && placed != scratch) sceKernelMunmap(placed, UNIT), gone = PS5_SHM_NO_PLACE;
            /* (the address may no longer be this GiB's to map over: units are zeroed at their place from here on) */
            if (gone) __atomic_store_n(&scratch_base, NULL, __ATOMIC_RELEASE);
        }
        placed = at;
    }
    r = sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;   /* (the reservation is still there) */
    if (r) { sceKernelReleaseDirectMemory(start, UNIT); return r; }
    if (!readable) memset(at, 0, UNIT);
    table[u % GIB_UNITS] = start + 1;
    set_bit(u, 1);
    if (zero_bit(u)) {   /* (the mapping of its own replaced the shared zeros') */
        set_zero_bit(u, 0);
        __atomic_sub_fetch(&st.zero_units, 1, __ATOMIC_RELAXED);
        ADD(zero_upgrades, 1);
        struct unit_note *const n = note_of(u);
        if (n->reclaimed) {   /* (the reclaim had put it there: written soon after, it is left alone for longer each time) */
            const uint16_t pass = (uint16_t)__atomic_load_n(&reclaim_pass, __ATOMIC_RELAXED);
            n->reclaimed = 0;
            ADD(reclaim_rebacked, 1);
            if (before(pass, n->until)) {
                if (n->flaps < FLAPS_MAX) n->flaps++;
                n->until = (uint16_t)(pass + 1 + (QUIET_PASSES << (n->flaps - 1)));
                ADD(reclaim_flaps, 1);
            } else n->flaps = 0, n->until = pass;
        }
    }
    const uint64_t now = ADD(backed_units, 1);
    uint64_t peak = __atomic_load_n(&st.peak_units, __ATOMIC_RELAXED);
    while (now > peak && !__atomic_compare_exchange_n(&st.peak_units, &peak, now, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return 0;
}

/* give unit u back (its GiB's lock held) */
static int32_t unback_unit(uint64_t u) {
    struct cold_note *const n = cnote(u);
    const unsigned s = cstate(n);
    if (!bit(u)) {
        if (!(s & (C_PARKED | C_SPILLED))) return 0;
        /* (2026-11-12) away: its place is a reservation already. Its memory, when it holds any, is given back (by the writer
           when it has it mapped at its staging address: C_DEAD), and a copy on the SSD is forgotten unread */
        int64_t *const table = meta[u / GIB_UNITS];
        const int held = (s & C_PARKED) || cold_mode == HOS_PS5_COLD_MEASURE;
        unsigned next = s & ~(C_PARKED | C_SPILLED | C_CLEAN | C_ZERO);
        if (held) {
            if (s & C_STAGED) next |= C_DEAD;
            else sceKernelReleaseDirectMemory(table[u % GIB_UNITS] - 1, UNIT);
            table[u % GIB_UNITS] = 0;
            __atomic_sub_fetch(&st.backed_units, 1, __ATOMIC_RELAXED);
        }
        if (s & C_PARKED) __atomic_sub_fetch(&st.cold_parked, 1, __ATOMIC_RELAXED);
        if (s & C_SPILLED) {
            __atomic_sub_fetch(&st.cold_spilled, 1, __ATOMIC_RELAXED);
            if (s & C_ZERO) __atomic_sub_fetch(&st.cold_spilled_zero, 1, __ATOMIC_RELAXED);
        }
        if (n->slot) ADD(cold_stale, 1);
        n->slot = 0;
        cset(n, next);
        n->gen++;
        ADD(cold_released, 1);
        ADD(decommit_units, 1);
        return 0;
    }
    int64_t *const table = meta[u / GIB_UNITS];
    const int64_t start = table ? table[u % GIB_UNITS] - 1 : -1;
    void *at = (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT)), *placed = at;
    /* (a reservation laid over the mapping replaces it: the range is never free for another reservation to take) */
    int32_t r = sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
    if (r == 0 && placed != at) { sceKernelMunmap(placed, UNIT); r = PS5_SHM_NO_PLACE; }
    if (r) {   /* (unmapped, then reserved again: what the platform's ps5_vrange_decommit does) */
        ADD(rereserve_fallbacks, 1);
        r = sceKernelMunmap(at, UNIT);
        if (r) return r;
        placed = at;
        r = sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
        if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT);
    }
    if (n && (s & C_STAGED) && !(s & C_DEAD)) cset(n, s | C_DEAD);   /* (the writer maps it: it gives it back) */
    else if (start >= 0) sceKernelReleaseDirectMemory(start, UNIT);
    if (table) table[u % GIB_UNITS] = 0;
    if (n) {   /* (a clean unit: its copy forgotten with it) */
        if (s & C_CLEAN) __atomic_sub_fetch(&st.cold_clean, 1, __ATOMIC_RELAXED);
        drop_copy(n);
        n->gen++;
    }
    set_bit(u, 0);
    __atomic_sub_fetch(&st.backed_units, 1, __ATOMIC_RELAXED);
    ADD(decommit_units, 1);
    return 0;
}

/* back units [lo, hi) of the window, GiB by GiB; *done: how many were. around: the rest of a fault's block, which leaves the
   units the reclaim put behind the shared zeros where they are (each gets its memory at its own write) */
static int32_t back_units(uint64_t lo, uint64_t hi, uint64_t *done, int around) {
    int32_t r = 0;
    uint64_t n = 0;
    for (uint64_t u = lo; u < hi && r == 0;) {
        const uint64_t g = u / GIB_UNITS, end = (g + 1) * GIB_UNITS < hi ? (g + 1) * GIB_UNITS : hi;
        while (u < end && bit(u)) u++;   /* (already backed: no lock, no kernel call) */
        if (u == end) continue;
        lock_gib(g);
        for (; u < end; u++) {
            if (bit(u)) continue;
            if (around && zero_bit(u) && meta[g] && note_of(u)->reclaimed) continue;
            if (cold_away(u)) {   /* (2026-11-12: away, its own bytes back for its own access; the rest of a block leaves it) */
                if (!around && (r = back_unit(u)) != 0) break;
                continue;
            }
            if ((r = back_unit(u)) != 0) break;
            n++;
        }
        unlock_gib(g);
    }
    if (done) *done = n;
    return r;
}

/* ---- the reservation ---- */
static uint64_t env_u64(const char *name, uint64_t otherwise) {
    const char *e = getenv(name);
    return e && *e ? strtoull(e, NULL, 0) : otherwise;
}

/* (the guest's address space: HOS_SPACE_END in host/hos.h on the console. 512 GiB as on the Switch 2, unless only a shorter
   window could be reserved (the prefix below): then regions are handed out down from where the window ends) */
uint64_t hos_space_end = (uint64_t)1 << 39;
/* the least a shorter window may be: the heap from 64 GiB and the VAMM region svcGetInfo 29/30 gives at 192..320 GiB
   (host/hos_svc.c), with room above it for the address regions */
#define MIN_PREFIX ((uint64_t)336 << 30)

static void release_pieces(uint64_t base, uint64_t upto, uint64_t piece) {
    for (uint64_t back = 0; back < upto; back += piece)
        ps5_vrange_release((void *)(uintptr_t)(base + back), (size_t)(upto - back < piece ? upto - back : piece));
}

/* (2026-11-10: the window is reserved by main's early call, before stage 4 reads hookshot_env.txt, so
   HOOKSHOT_PS5_FAULT_KB set in that file was never seen: a "measurement" of it measured nothing. Called once the file is
   read: the fault's block from the environment as it is now; the block's bytes are returned) */
uint64_t hos_ps5_guest_settings(void) {
    const uint64_t kb = env_u64("HOOKSHOT_PS5_FAULT_KB", fault_bytes >> 10);
    if (kb >= 64 && kb <= 65536 && !(kb & (kb - 1))) fault_bytes = kb << 10;
    if (zero_mode == 0) {   /* (the unit of zeros read faults map: made here, once the settings are read) */
        int mode = -1;
        if (env_u64("HOOKSHOT_PS5_ZERO_READS", 1) && guest_hi) {
            int64_t start = -1;
            void *at = NULL;
            if (allocate(UNIT, &start) == 0) {
                if (sceKernelMapDirectMemory(&at, UNIT, PROT_RW, 0, start, PS5_KERNEL_PAGE_SIZE) == 0 && at) {
                    memset(at, 0, UNIT);
                    sceKernelMunmap(at, UNIT);
                    zero_start = start;
                    /* (where back_unit zeroes the new memory of a unit behind these zeros: a unit of address space per GiB.
                       Refused: zeroed at its place, as before) */
                    void *m = NULL;
                    if (!scratch_base && ps5_vrange_reserve((size_t)(GIBS * UNIT), NULL, UNIT, &m) == 0)
                        __atomic_store_n(&scratch_base, (uint8_t *)m, __ATOMIC_RELEASE);
                    mode = 1;
                } else sceKernelReleaseDirectMemory(start, UNIT);
            }
        }
        __atomic_store_n(&zero_mode, mode, __ATOMIC_RELEASE);
    }
    return fault_bytes;
}
int hos_ps5_guest_zero_reads(void) { return __atomic_load_n(&zero_mode, __ATOMIC_ACQUIRE) == 1; }
int hos_ps5_guest_reserve(uint64_t base, uint64_t size) {
    if (!size || size > WINDOW_MAX || base % UNIT || size % UNIT) return 0;
    /* (main's early call made it: hos_main.c's again, for the same base, gets the window made, shorter or not) */
    if (guest_hi) return guest_lo == base && guest_hi <= base + size;
    direct_size = sceKernelGetDirectMemorySize();
    const uint64_t kb = env_u64("HOOKSHOT_PS5_FAULT_KB", 1024);
    if (kb >= 64 && kb <= 65536 && !(kb & (kb - 1))) fault_bytes = kb << 10;
    /* (the per-GiB tables' range: reserved now, mapped GiB by GiB as the window is backed) */
    if (!meta_base) {
        void *m = NULL;
        if (ps5_vrange_reserve((size_t)(GIBS * META_BYTES), NULL, UNIT, &m)) return 0;
        meta_base = (uint8_t *)m;
    }
    /* HOOKSHOT_PS5_WINDOW: whole, pieces or prefix, to take one way only (the probe's); unset: each in that order */
    const char *how = getenv("HOOKSHOT_PS5_WINDOW");
    const int only_whole = how && !strcmp(how, "whole"), only_pieces = how && !strcmp(how, "pieces"),
              only_prefix = how && !strcmp(how, "prefix");
    uint64_t piece = env_u64("HOOKSHOT_PS5_WINDOW_PIECE_GB", 16) << 30;
    if (!piece || piece % UNIT) piece = (uint64_t)16 << 30;
    window_piece_bytes = piece;
    /* 1. one reservation (plan A) */
    whole_result = only_pieces || only_prefix ? 1 : ps5_vrange_reserve_at((void *)(uintptr_t)base, (size_t)size);
    if (whole_result == 0) {
        window_mode = HOS_PS5_WINDOW_WHOLE, window_pieces = 1, window_piece_bytes = size;
        guest_lo = base, guest_hi = base + size, hos_space_end = size;
        return 1;
    }
    if (only_whole) return 0;
    /* 2. adjacent pieces of 16 GiB, the size PROBE.md measured granted at the hint (plan B'): the same addresses, so the
       same layout; 3. when the window's top is refused (the console's user map ends below 1 TiB: the PS4's ended at
       0xFC_0000_0000), the pieces granted from the base up, if they hold the heap and the VAMM region (a shorter address
       space for the guest: HOS_SPACE_END) */
    uint64_t at = 0;
    unsigned n = 0;
    for (; at < size; at += piece, n++) {
        const uint64_t bytes = size - at < piece ? size - at : piece;
        if (ps5_vrange_reserve_at((void *)(uintptr_t)(base + at), (size_t)bytes)) break;
    }
    window_pieces = n;
    if (at >= size && !only_prefix) {
        window_mode = HOS_PS5_WINDOW_PIECES;
        guest_lo = base, guest_hi = base + size, hos_space_end = size;
        return 1;
    }
    if (at >= size) at = size;
    if (only_pieces || at < MIN_PREFIX) {
        release_pieces(base, at, piece);
        return 0;
    }
    window_mode = HOS_PS5_WINDOW_PREFIX;
    guest_lo = base, guest_hi = base + at, hos_space_end = at;
    return 1;
}

int hos_ps5_in_guest(const void *p, size_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p;
    return guest_hi && a >= guest_lo && a + n <= guest_hi && a + n >= a;
}

int hos_ps5_guest_commit(void *p, size_t n) {
    if (!n) return 0;
    if (!hos_ps5_in_guest(p, n)) return PS5_SHM_BAD_REQUEST;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo;
    const uint64_t lo = a >> UNIT_SHIFT, hi = (a + n + UNIT - 1) >> UNIT_SHIFT;
    uint64_t done = 0;
    int32_t r = 0;
    /* (2026-11-10) Every unit under its lock, the backed ones too: the reclaim may hold one read-only at this moment, or have
       put it behind the shared zeros since it was last written, and a commit is what a system call's write into guest memory
       relies on (it takes no fault). So each unit is backed if it is not, and held: the passes started before `until` leave
       it alone, HOLD_PASSES whole intervals at least */
    const uint16_t until = (uint16_t)(__atomic_load_n(&reclaim_pass, __ATOMIC_RELAXED) + 1 + HOLD_PASSES);
    /* (2026-11-12) and a cold unit: brought back if it is away (from the SSD, in this thread), made read-write if it is clean,
       and not parked again for HOOKSHOT_PS5_COLD_HOLD_S seconds (the system call that follows takes no fault) */
    const uint16_t hold = __atomic_load_n(&cold_clock0, __ATOMIC_ACQUIRE) ? (uint16_t)(cold_s16() + ck.hold_s + (ck.hold_s != 0)) : 0;
    for (uint64_t u = lo; u < hi && r == 0;) {
        const uint64_t g = u / GIB_UNITS, end = (g + 1) * GIB_UNITS < hi ? (g + 1) * GIB_UNITS : hi;
        lock_gib(g);
        for (; u < end; u++) {
            struct cold_note *const cn = cnote(u);
            if (cn && (r = cold_writable(u)) != 0) break;
            if (!bit(u)) {
                if ((r = back_unit(u)) != 0) break;
                done++;
            }
            if (cn && before(cn->hold, hold)) cn->hold = hold;
            struct unit_note *const note = note_of(u);
            if (before(note->until, until)) note->until = until;
        }
        unlock_gib(g);
    }
    ADD(commits, 1);
    ADD(commit_units, done);
    if (r) {
        ADD(commit_failures, 1);
        __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
    }
    return r;
}

int hos_ps5_guest_pin(void *p, size_t n) {
    if (!n) return 0;
    if (!hos_ps5_in_guest(p, n)) return PS5_SHM_BAD_REQUEST;
    if (pin_mode < 0) {   /* (never from the fault handler: the renderer's threads, after the settings file was read) */
        const char *e = getenv("HOOKSHOT_PS5_GPU_PIN");
        /* (before any lock is taken below: with no pins the reclaim of zero units stops, and it reads this under each lock) */
        __atomic_store_n(&pin_mode, !(e && e[0] == '0'), __ATOMIC_SEQ_CST);
    }
    const int pin = __atomic_load_n(&pin_mode, __ATOMIC_RELAXED) != 0;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo;
    const uint64_t lo = a >> UNIT_SHIFT, hi = (a + n + UNIT - 1) >> UNIT_SHIFT;
    int32_t r = 0;
    uint64_t fresh = 0, newly = 0;
    /* (backed and pinned under the one lock a decommit takes: from here no unit of the range can be given back) */
    for (uint64_t u = lo; u < hi && r == 0;) {
        const uint64_t g = u / GIB_UNITS, end = (g + 1) * GIB_UNITS < hi ? (g + 1) * GIB_UNITS : hi;
        lock_gib(g);
        for (; u < end; u++) {
            /* (2026-11-12: a cold unit is brought back read-write, its copy forgotten, before the pin and the GPU's grant:
               the import's protection would make a clean unit writable with no fault to say it. A cold unit is never pinned,
               and the cold thread never parks a pinned one: both under this lock) */
            if (cnote(u) && (r = cold_writable(u)) != 0) break;
            if (!bit(u)) {
                if ((r = back_unit(u)) != 0) break;
                fresh++;
            }
            if (pin && !pin_bit(u)) {
                __atomic_fetch_or(&pinned[u >> 3], (uint8_t)(1u << (u & 7)), __ATOMIC_RELEASE);
                newly++;
            }
        }
        unlock_gib(g);
    }
    ADD(pins, 1);
    ADD(pinned_units, newly);
    ADD(pin_backed_units, fresh);
    if (r) {
        ADD(pin_failures, 1);
        __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
    }
    return r;
}

int hos_ps5_guest_pinned(const void *p, size_t n) {
    if (!n || !hos_ps5_in_guest(p, n)) return 0;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo;
    for (uint64_t u = a >> UNIT_SHIFT; u < (a + n + UNIT - 1) >> UNIT_SHIFT; u++)
        if (pin_bit(u)) return 1;
    return 0;
}

/* (2026-11-10) The renderer's, before it uploads a texture from [p, p + n) (gpu/vk/vk_texture.cpp upload_zeros): 1 when no
   unit of the range has memory of its own, so that every byte of it reads zero: units nothing touched since the window was
   reserved or since they were last given back, and units behind the shared zeros (read and never written, or found all
   zeros by the reclaim, which looks at a unit read-only and under this lock). The shared zeros made
   the read of such storage cheap; the renderer still staged and copied all of it (the 12288 x 12288 shadow map: a staging
   buffer of 288 MiB for zeros). Each GiB's units are looked at under its lock, the one a unit is given its memory under
   (back_unit maps it writable before it sets the bit): a 1 says that no unit of the range was writable at that moment, so
   no write had landed in it. A write that comes after is the write watch's to report, as after any upload. A unit backed
   and never written (a commit, a pin, the rest of a fault's block) answers 0: the caller then reads it, as before. */
int hos_ps5_guest_untouched(const void *p, size_t n) {
    if (!n || !hos_ps5_in_guest(p, n)) return 0;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo;
    const uint64_t lo = a >> UNIT_SHIFT, hi = (a + n + UNIT - 1) >> UNIT_SHIFT;
    int untouched = 1;
    for (uint64_t u = lo; u < hi && untouched;) {
        const uint64_t g = u / GIB_UNITS, end = (g + 1) * GIB_UNITS < hi ? (g + 1) * GIB_UNITS : hi;
        lock_gib(g);
        const int cold = __atomic_load_n(&cold_notes[g], __ATOMIC_ACQUIRE) != NULL;   /* (2026-11-12: a unit away holds data) */
        while (u < end) {
            if (!cold && !(u & 7) && u + 8 <= end) {   /* (eight units a load) */
                if (__atomic_load_n(&backed[u >> 3], __ATOMIC_ACQUIRE)) { untouched = 0; break; }
                u += 8;
            } else {
                if (has_data(u)) { untouched = 0; break; }
                u++;
            }
        }
        unlock_gib(g);
    }
    return untouched;
}

/* (2026-11-10) The renderer's count, for its report of what the textures' storage holds of the window (gpu/vk/vk_texture.cpp
   storage_report). A texture exists twice here: the game's copy in the window and the image the renderer made of it, and
   nothing said how much of the window's 7 GiB the first is. The bytes of [p, p + n) in units with memory of their own (a
   unit only partly inside counts for its part); of them, those in pinned units, which a decommit zeroes and keeps; and what
   a decommit of the range would give back: the units whole inside it, backed and not pinned (hos_ps5_guest_decommit's own
   rule). The bits are read as they are, eight units a load where the range covers them: no lock is taken, so a fault never
   waits for a report, and a unit backed or given back meanwhile is counted either way. Nothing is backed, mapped or
   changed, and the memory itself is not read. */
uint64_t hos_ps5_guest_backed_bytes(const void *p, size_t n, uint64_t *pinned_bytes, uint64_t *free_bytes) {
    if (pinned_bytes) *pinned_bytes = 0;
    if (free_bytes) *free_bytes = 0;
    if (!n || !hos_ps5_in_guest(p, n)) return 0;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo, end = a + n;
    uint64_t bytes = 0, held = 0, whole = 0;
    for (uint64_t u = a >> UNIT_SHIFT; u < (end + UNIT - 1) >> UNIT_SHIFT;) {
        const uint64_t ulo = u << UNIT_SHIFT, uhi = ulo + UNIT;
        /* (2026-11-12: in a GiB with cold notes, unit by unit: a parked unit holds its memory, which a decommit gives back,
           and is never pinned; one on the SSD holds none, but in the measurement) */
        if (__atomic_load_n(&cold_notes[u / GIB_UNITS], __ATOMIC_RELAXED)) {
            if (holds_memory(u)) {
                const uint64_t part = (uhi < end ? uhi : end) - (ulo > a ? ulo : a);
                bytes += part;
                if (pin_bit(u)) held += part;
                else if (part == UNIT) whole += UNIT;
            }
            u++;
            continue;
        }
        if (!(u & 7) && ulo >= a && ulo + 8 * UNIT <= end) {   /* (eight units whole inside the range: a byte of each table) */
            const unsigned b = __atomic_load_n(&backed[u >> 3], __ATOMIC_RELAXED);
            const unsigned k = b & __atomic_load_n(&pinned[u >> 3], __ATOMIC_RELAXED);
            bytes += (uint64_t)__builtin_popcount(b) << UNIT_SHIFT;
            held += (uint64_t)__builtin_popcount(k) << UNIT_SHIFT;
            whole += (uint64_t)__builtin_popcount(b & ~k) << UNIT_SHIFT;
            u += 8;
            continue;
        }
        if (bit(u)) {
            const uint64_t part = (uhi < end ? uhi : end) - (ulo > a ? ulo : a);
            const int pin = pin_bit(u);
            bytes += part;
            if (pin) held += part;
            else if (part == UNIT) whole += UNIT;
        }
        u++;
    }
    if (pinned_bytes) *pinned_bytes = held;
    if (free_bytes) *free_bytes = whole;
    return bytes;
}

int hos_ps5_guest_decommit(void *p, size_t n) {
    if (!n) return 0;
    if (!hos_ps5_in_guest(p, n)) return PS5_SHM_BAD_REQUEST;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo, end = a + n;
    int32_t r = 0;
    uint64_t kept = 0;
    for (uint64_t u = a >> UNIT_SHIFT; u < (end + UNIT - 1) >> UNIT_SHIFT && r == 0; u++) {
        if (!has_data(u)) continue;   /* (2026-11-12: a unit away has data too) */
        const uint64_t ulo = u << UNIT_SHIFT, uhi = ulo + UNIT;
        lock_gib(u / GIB_UNITS);
        if (ulo >= a && uhi <= end && bit(u) && pin_bit(u)) {   /* (the GPU may reach it: zeroed and kept, its mapping as it is) */
            memset((void *)(uintptr_t)(guest_lo + ulo), 0, UNIT);
            kept++;
        } else if (ulo >= a && uhi <= end) r = unback_unit(u);   /* (away: forgotten, never read back) */
        else if (has_data(u) && (r = cold_writable(u)) == 0) {
            /* (partly inside: its part zeroed, the unit kept. A cold unit is brought back read-write first: the memset must not
               fault with the lock held) */
            const uint64_t from = ulo > a ? ulo : a, to = uhi < end ? uhi : end;
            memset((void *)(uintptr_t)(guest_lo + from), 0, (size_t)(to - from));
        }
        unlock_gib(u / GIB_UNITS);
    }
    if (kept) {
        ADD(pinned_kept_units, kept);
        ADD(pinned_kept_calls, 1);
    }
    return r;
}

int hos_ps5_guest_committed(const void *p, size_t *run) {
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo, end = guest_hi - guest_lo;
    /* (2026-11-12: a unit away is committed: it has data, which its first access brings back) */
    const int state = has_data(a >> UNIT_SHIFT);
    uint64_t at = (a & ~(UNIT - 1)) + UNIT;
    while (at < end && has_data(at >> UNIT_SHIFT) == state && at - a < ((uint64_t)1 << 30)) at += UNIT;   /* (runs reported up to 1 GiB) */
    if (run) *run = (size_t)((at < end ? at : end) - a);
    return state;
}

int hos_ps5_guest_fault2(const void *p, int write);
int hos_ps5_guest_fault(const void *p) { return hos_ps5_guest_fault2(p, 1); }
/* write: 0 when the fault is known to be a read (the shared zeros may answer it) */
/* (2026-11-12) a cold unit, its GiB's lock held: 1 answered (the access is retried), 0 not a cold unit (the paths below). Away:
   its bytes back (from the SSD when they are there), read-only for a read when it has its copy. Clean (backed read-only): a
   fault there is a write, or a read that met it while it was away; either way it is made read-write and its copy forgotten,
   so that the retry goes through whatever the fault's sense said */
static int cold_fault(uint64_t u, int write) {
    struct cold_note *const n = cnote(u);
    const unsigned s = cstate(n);
    if (s & (C_PARKED | C_SPILLED)) {
        cold_back(u, n, write);
        return 1;
    }
    if ((s & C_CLEAN) && bit(u)) {
        const int32_t r = make_writable(u, n);
        if (r) cold_fatal(u, "a read-only unit could not be made writable", r);
        return 1;
    }
    return 0;
}
int hos_ps5_guest_fault2(const void *p, int write) {
    if (!hos_ps5_in_guest(p, 1)) return 0;
    const uint64_t a = (uint64_t)(uintptr_t)p - guest_lo, u = a >> UNIT_SHIFT;
    /* (2026-11-12) cold units first (HOOKSHOT_PS5_COLD): the test without the lock is a hint, each path below asks again
       under it. With the cold units off, no GiB has notes: one pointer read */
    if (cstate(cnote(u)) & (C_PARKED | C_SPILLED | C_CLEAN)) {
        const int saved = errno;   /* (the interrupted code's: the kernel calls below may set it) */
        lock_gib(u / GIB_UNITS);
        const int answered = cold_fault(u, write);
        unlock_gib(u / GIB_UNITS);
        errno = saved;
        if (answered) return 1;
    }
    if (bit(u)) {
        /* A fault on a unit that has memory of its own: another thread backed it since the access; or the reclaim of zero
           units holds it read-only while it reads it (a write, then); or this is a protection fault on a backed unit, which is
           not the window's to answer. Its GiB's lock says which: under it no unit is half way. (2026-11-10: the kernel was
           asked for the protection at once and without the lock, and a write that met the reclaim's read-only unit, or any
           access that met a decommit between its reservation and its bit, would have been refused) */
        int answer = -1;   /* (-1: no longer backed, the reclaim or a decommit took its memory: answered below as any such) */
        lock_gib(u / GIB_UNITS);
        if (cold_fault(u, write)) answer = 1;   /* (2026-11-12: made clean or parked since the test above) */
        else if (bit(u)) {
            void *s = NULL, *e = NULL;
            uint32_t prot = 0;
            const uintptr_t page = (uintptr_t)p & ~(uintptr_t)(PS5_KERNEL_PAGE_SIZE - 1);
            /* (a query the kernel refuses says nothing: retried, as the race is what a backed unit almost always means) */
            answer = sceKernelQueryMemoryProtection((void *)page, &s, &e, &prot) != 0 || (prot & PROT_RW) == PROT_RW;
        }
        unlock_gib(u / GIB_UNITS);
        if (answer >= 0) {
            if (answer) ADD(fault_retries, 1);
            else ADD(fault_refused, 1);
            return answer;
        }
    }
    if (!bit(u) && !write && !zero_bit(u) && __atomic_load_n(&zero_mode, __ATOMIC_ACQUIRE) == 1) {
        /* a read of a unit nothing wrote: the shared zeros, read-only */
        int32_t r = 0;
        lock_gib(u / GIB_UNITS);
        if (cold_fault(u, write)) r = 0;   /* (2026-11-12: away since the test above: its own bytes, never the zeros) */
        else if (!bit(u) && !zero_bit(u)) {
            void *at = (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT)), *placed = at;
            r = sceKernelMapDirectMemory(&placed, UNIT, PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_MAP_FIXED, zero_start, PS5_KERNEL_PAGE_SIZE);
            if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
            if (r == 0) {
                set_zero_bit(u, 1);
                ADD(zero_units, 1);
                ADD(zero_maps, 1);
            } else {   /* (not on this console: every unit its own memory from here, as before) */
                ADD(zero_failures, 1);
                __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
                __atomic_store_n(&zero_mode, -1, __ATOMIC_RELEASE);
            }
        }
        unlock_gib(u / GIB_UNITS);
        if (r == 0) return 1;
    }
    /* (a fault on a unit of shared zeros is a write, or a read that raced its mapping: memory of its own either way, below.
       A unit another thread backs meanwhile is found backed under the lock, and the access retried) */
    const uint64_t t0 = now_ns();
    uint64_t done = 0, more = 0;
    /* its own unit first, then the rest of the block around it (best effort: the access needs only the first) */
    int32_t r = back_units(u, u + 1, &done, 0);
    if (r == 0) {
        const uint64_t blo = (a & ~(fault_bytes - 1)) >> UNIT_SHIFT;
        uint64_t bhi = ((a & ~(fault_bytes - 1)) + fault_bytes) >> UNIT_SHIFT;
        if (bhi > (guest_hi - guest_lo) >> UNIT_SHIFT) bhi = (guest_hi - guest_lo) >> UNIT_SHIFT;
        back_units(blo, bhi, &more, 1);
    }
    const uint64_t ns = now_ns() - t0;
    const uint64_t k = ADD(faults, 1);
    ADD(fault_units, done + more);
    ADD(fault_ns, ns);
    uint64_t max = __atomic_load_n(&st.fault_ns_max, __ATOMIC_RELAXED);
    while (ns > max && !__atomic_compare_exchange_n(&st.fault_ns_max, &max, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (r) {
        ADD(fault_failures, 1);
        __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
        say_fault(": first touch, backing refused after ", a, done + more, r, ns);
        return 0;
    }
    if (k <= 16) say_fault(" backed on first touch: ", a, done + more, 0, ns);
    return 1;
}

/* (2026-11-10) What the backed units hold. The window is 7.3 GiB of the title's 12 where the memory floor ends a session, and
   nothing said what is in it: units the game wrote zeros into (cleared heaps) could go back behind the shared zeros, and a
   fill pattern says a heap is filled when made. Each backed unit is read until a word differs from its first, so one that
   holds data costs a few words and one that is all one word is read whole. Read while the game runs: a count, not a snapshot.
   A unit given back meanwhile takes the read fault any read of unbacked memory takes. */
void hos_ps5_guest_contents(char *buf, size_t cap, uint64_t *backed_mib, uint64_t *zero_mib, uint64_t *same_mib) {
    uint64_t tb = 0, tz = 0, ts = 0;
    size_t at = 0;
    if (cap) buf[0] = 0;
    const uint64_t units = (guest_hi - guest_lo) >> UNIT_SHIFT;
    for (uint64_t g = 0; g < GIBS && g * GIB_UNITS < units; g++) {
        uint64_t b = 0, z = 0, r = 0;
        for (uint64_t u = g * GIB_UNITS; u < (g + 1) * GIB_UNITS && u < units; u++) {
            if (!(u & 7) && !__atomic_load_n(&backed[u >> 3], __ATOMIC_RELAXED)) { u += 7; continue; }
            if (!bit(u) || zero_bit(u)) continue;
            b++;
            const volatile uint64_t *w = (const volatile uint64_t *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT));
            const uint64_t first = w[0];
            size_t i = 1;
            while (i < UNIT / 8 && w[i] == first) i++;
            if (i == UNIT / 8) { if (first) r++; else z++; }
        }
        if (b && at + 64 < cap)
            at += (size_t)snprintf(buf + at, cap - at, "%s%llu: %llu/%llu/%llu", at ? "; " : "", (unsigned long long)g,
                                   (unsigned long long)(b >> 4), (unsigned long long)(z >> 4), (unsigned long long)(r >> 4));
        tb += b, tz += z, ts += r;
    }
    *backed_mib = tb >> 4, *zero_mib = tz >> 4, *same_mib = ts >> 4;
}

/* ---- zero units put back behind the shared zeros (the file's header has the sequence and why no write is lost) ---- */
/* unit u as it reads now, the game running: a hint, the lock and the protection come after */
static int reads_zero(uint64_t u) {
    const volatile uint64_t *w = (const volatile uint64_t *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT));
    for (size_t i = 0; i < UNIT / 8; i++)
        if (w[i]) return 0;
    return 1;
}
/* a unit nothing can write (read-only, its lock held) */
static int all_zero(const void *at) {
    const uint64_t *w = (const uint64_t *)at;
    for (size_t i = 0; i < UNIT / 8; i += 8)
        if (w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7]) return 0;
    return 1;
}
/* unit u read-write again (its lock held, its own memory still its own): the protection, or should the kernel refuse that,
   its memory mapped again over what is there, as back_unit maps it */
static int32_t writable_again(uint64_t u) {
    void *at = (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT)), *placed = at;
    int32_t r = sceKernelMprotect(at, UNIT, PROT_RW);
    if (r == 0) return 0;
    r = sceKernelMapDirectMemory(&placed, UNIT, PROT_RW, PS5_KERNEL_MAP_FIXED, meta[u / GIB_UNITS][u % GIB_UNITS] - 1, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
    return r;
}
/* the kernel refused a call the reclaim needs: no more passes this session (step: 1 the protection, 2 read-write again,
   3 the shared zeros' mapping, 4 the protection made but the kernel still says the unit is writable) */
static void reclaim_refused(int step, int32_t error) {
    ADD(reclaim_failures, 1);
    __atomic_store_n(&st.last_error, error, __ATOMIC_RELAXED);
    reclaim_error = error, reclaim_error_step = step;
    __atomic_store_n(&reclaim_mode, -2, __ATOMIC_RELEASE);
}
/* unit u, backed, not pinned, not held, its GiB's lock held: behind the shared zeros if every byte of it is zero.
   1 put back; 0 kept (written since it was looked at: *written; not the CPU's alone for the kernel: *foreign); -1 the kernel
   refused a call (said, the reclaim off; the unit is as it was unless step 2 is what failed) */
static int reclaim_unit(uint64_t u, uint16_t pass, struct hos_ps5_guest_reclaim *o) {
    int64_t *const table = meta[u / GIB_UNITS];
    struct unit_note *const note = note_of(u);
    void *const at = (void *)(uintptr_t)(guest_lo + (u << UNIT_SHIFT));
    /* what the kernel has of it: GPU access is an import nobody pinned, and anything but read-write is somebody else's
       protection (a debugging watch); neither is touched, nor asked about again. (2026-11-12) Nor is a unit the kernel does
       not answer for, or whose answer does not cover all of it (a grant or a protection over part of the unit: the kernel's
       range then ends inside it): a doubt keeps the unit */
    void *s = NULL, *e = NULL;
    uint32_t prot = 0;
    const int answered = sceKernelQueryMemoryProtection(at, &s, &e, &prot) == 0;
    if (!answered || (uintptr_t)s > (uintptr_t)at || (uintptr_t)e < (uintptr_t)at + UNIT || (prot & PROT_RW) != PROT_RW ||
        (prot & PROT_GPU)) {
        note->until = (uint16_t)(pass + FOREIGN_PASSES);
        ADD(reclaim_foreign, 1);
        o->foreign++;
        o->unanswered += !answered;
        return 0;
    }
    /* read-only: a write from here on faults, and its handler waits for the lock this thread holds */
    int32_t r = sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ);
    if (r) { reclaim_refused(1, r); return -1; }
    /* (2026-11-12) and the kernel asked again: everything below rests on no write landing from here, and the console never
       had a window unit made read-only before. Still writable, or no answer: read-write again, and no more passes */
    s = e = NULL, prot = 0;
    if (sceKernelQueryMemoryProtection(at, &s, &e, &prot) != 0 || (uintptr_t)s > (uintptr_t)at || (uintptr_t)e < (uintptr_t)at + UNIT ||
        (prot & PS5_KERNEL_PROT_CPU_WRITE)) {
        const int32_t back = writable_again(u);
        reclaim_refused(back ? 2 : 4, back ? back : (int32_t)prot);
        return -1;
    }
    RECLAIM_HOOK(1, u);
    if (!all_zero(at)) {   /* (written between the first look and the protection: kept) */
        if ((r = writable_again(u)) != 0) { reclaim_refused(2, r); return -1; }
        ADD(reclaim_written, 1);
        o->written++;
        return 0;
    }
    /* the shared zeros over it (the map replaces the mapping, as back_unit's replaces the zeros'), then its memory back */
    void *placed = at;
    r = sceKernelMapDirectMemory(&placed, UNIT, PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_MAP_FIXED, zero_start, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
    if (r) {
        const int32_t back = writable_again(u);
        reclaim_refused(back ? 2 : 3, back ? back : r);
        return -1;
    }
    sceKernelReleaseDirectMemory(table[u % GIB_UNITS] - 1, UNIT);
    table[u % GIB_UNITS] = 0;
    set_zero_bit(u, 1);
    set_bit(u, 0);
    __atomic_sub_fetch(&st.backed_units, 1, __ATOMIC_RELAXED);
    ADD(zero_units, 1);
    ADD(reclaim_units, 1);
    note->reclaimed = 1;
    note->until = (uint16_t)(pass + 1 + SOON_PASSES);
    o->reclaimed++;
    return 1;
}
/* (2026-11-12) The direct memory a unit gives back is a piece of 64 KiB, and pieces the reclaim frees one by one among units
   that stay are free pieces of their own. The memory watch walks them every 50 ms and gives up past 8192 questions to the
   kernel (one or two a piece: host/posix/ps5_early.c hos_ps5_memory_free); the largest piece alone then stands for the free
   memory, and the title ends at its floor early. 675 pieces at most in the runs of 2026-11-12. So a pass stops putting units
   back when the watch's last count, and one more piece for each unit put back since that count changed, would pass
   PIECES_MAX, and makes no pass at all when a walk failed since the last one. The watch's count is ps5_early.c's: weak, as
   the platform probe and the kernel model link this file without it (the model's test gives its own) */
uint64_t hos_ps5_memory_pieces(uint64_t *no_walk) __attribute__((weak));
#define PIECES_MAX 3000
static uint64_t reclaim_walks;   /* (the measures without their walk at the end of the last pass: one pass at a time) */
static int too_many_pieces(uint64_t *seen, uint64_t *since, uint64_t walks, uint64_t *pieces) {
    if (!hos_ps5_memory_pieces) return 0;
    uint64_t no_walk = 0;
    const uint64_t p = hos_ps5_memory_pieces(&no_walk);
    if (p != *seen) *seen = p, *since = 0;
    *pieces = p;
    return no_walk != walks || p + *since >= PIECES_MAX;
}
/* what stops the reclaim, or HOS_PS5_RECLAIM_RAN */
static int reclaim_state(void) {
    const int mode = __atomic_load_n(&reclaim_mode, __ATOMIC_ACQUIRE);
    if (mode == -1) return HOS_PS5_RECLAIM_OFF;
    if (mode == -2) return HOS_PS5_RECLAIM_REFUSED;
    if (__atomic_load_n(&zero_mode, __ATOMIC_ACQUIRE) != 1) return HOS_PS5_RECLAIM_NO_ZEROS;
    /* (no pins: the window does not know what the GPU reaches. Read under each unit's lock too: hos_ps5_guest_pin writes it
       before it takes any) */
    if (__atomic_load_n(&pin_mode, __ATOMIC_SEQ_CST) == 0) return HOS_PS5_RECLAIM_NO_PINS;
    return HOS_PS5_RECLAIM_RAN;
}
int hos_ps5_guest_reclaim(struct hos_ps5_guest_reclaim *o) {
    struct hos_ps5_guest_reclaim mine;
    if (!o) o = &mine;
    memset(o, 0, sizeof *o);
    if (__atomic_load_n(&reclaim_mode, __ATOMIC_ACQUIRE) == 0) {
        /* (read here, at the first pass, a minute into the run: a static initializer runs before main reads the console's
           settings file. 2026-11-12: off unless asked for, HOOKSHOT_PS5_ZERO_RECLAIM=1) */
        const char *e = getenv("HOOKSHOT_PS5_ZERO_RECLAIM");
        int unread = 0;
        __atomic_compare_exchange_n(&reclaim_mode, &unread, e && e[0] && e[0] != '0' ? 1 : -1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    o->state = guest_hi ? reclaim_state() : HOS_PS5_RECLAIM_NO_ZEROS;
    if (o->state == HOS_PS5_RECLAIM_RAN && __atomic_exchange_n(&reclaim_busy, 1, __ATOMIC_ACQUIRE)) o->state = HOS_PS5_RECLAIM_BUSY;
    if (o->state == HOS_PS5_RECLAIM_RAN) {
        const uint64_t t0 = now_ns();
        const uint16_t pass = (uint16_t)__atomic_add_fetch(&reclaim_pass, 1, __ATOMIC_RELAXED);
        const uint64_t units = (guest_hi - guest_lo) >> UNIT_SHIFT;
        int state = HOS_PS5_RECLAIM_RAN, crowded = 0;
        uint64_t seen = UINT64_MAX, since = 0;
        for (uint64_t g = 0; g < GIBS && g * GIB_UNITS < units && state == HOS_PS5_RECLAIM_RAN && !crowded; g++) {
            if (!__atomic_load_n(&meta[g], __ATOMIC_ACQUIRE)) continue;   /* (no unit of this GiB was ever backed) */
            for (uint64_t u = g * GIB_UNITS; u < (g + 1) * GIB_UNITS && u < units; u++) {
                if (!(u & 7) && !__atomic_load_n(&backed[u >> 3], __ATOMIC_RELAXED)) { u += 7; continue; }
                if (!bit(u)) continue;
                o->looked++;
                /* (hints, without the lock: all of it is asked again under it) */
                if (pin_bit(u)) { o->pinned++; continue; }
                if (before(pass, __atomic_load_n(&note_of(u)->until, __ATOMIC_RELAXED))) { o->held++; continue; }
                if (cstate(cnote(u)) & (C_CLEAN | C_STAGED)) { o->held++; continue; }   /* (2026-11-12: the cold thread's) */
                if (!reads_zero(u)) continue;   /* (a unit that holds data costs a few words) */
                o->zeros++;
                if (too_many_pieces(&seen, &since, reclaim_walks, &o->pieces)) { o->crowded = crowded = 1; break; }
                RECLAIM_HOOK(0, u);
                /* the lock for this unit alone: a fault in this GiB waits for one unit's system calls, not for a scan */
                lock_gib(g);
                const uint64_t l0 = now_ns();
                int did = 0;
                state = reclaim_state();
                if (state == HOS_PS5_RECLAIM_RAN && bit(u) && !pin_bit(u) && !before(pass, note_of(u)->until) &&
                    !(cstate(cnote(u)) & (C_CLEAN | C_STAGED | C_PARKED | C_SPILLED)))
                    did = reclaim_unit(u, pass, o);
                else o->changed++;
                const uint64_t held_ns = now_ns() - l0;
                unlock_gib(g);
                if (held_ns > o->lock_ns_max) o->lock_ns_max = held_ns;
                if (did < 0) state = HOS_PS5_RECLAIM_REFUSED;
                since += did > 0;   /* (a piece more, at most, until the watch counts again) */
                if (state != HOS_PS5_RECLAIM_RAN) break;
            }
        }
        /* (a pass a refusal or the pins' switch cut short still says what it did: state stays "ran", the next call says why
           there are no more) */
        if (hos_ps5_memory_pieces) hos_ps5_memory_pieces(&reclaim_walks);
        o->pass = ADD(reclaim_passes, 1);
        o->ns = now_ns() - t0;
        static uint64_t rebacked_before;   /* (one pass at a time: reclaim_busy) */
        const uint64_t rebacked = __atomic_load_n(&st.reclaim_rebacked, __ATOMIC_RELAXED);
        o->rebacked = rebacked - rebacked_before;
        rebacked_before = rebacked;
        __atomic_store_n(&reclaim_busy, 0, __ATOMIC_RELEASE);
    }
    o->error = reclaim_error, o->error_step = reclaim_error_step;
    o->total_reclaimed = __atomic_load_n(&st.reclaim_units, __ATOMIC_RELAXED);
    o->total_rebacked = __atomic_load_n(&st.reclaim_rebacked, __ATOMIC_RELAXED);
    o->total_flaps = __atomic_load_n(&st.reclaim_flaps, __ATOMIC_RELAXED);
    o->zero_units = __atomic_load_n(&st.zero_units, __ATOMIC_RELAXED);
    return o->state == HOS_PS5_RECLAIM_RAN;
}
void hos_ps5_guest_stats(struct hos_ps5_guest_stats *s) {
    memset(s, 0, sizeof *s);
    s->base = guest_lo, s->size = guest_hi - guest_lo;
    s->mode = window_mode, s->whole_result = whole_result, s->pieces = window_pieces, s->piece_bytes = window_piece_bytes;
    s->space_end = hos_space_end;
    s->fault_bytes = fault_bytes;
    s->backed_bytes = __atomic_load_n(&st.backed_units, __ATOMIC_RELAXED) << UNIT_SHIFT;
    s->peak_backed_bytes = __atomic_load_n(&st.peak_units, __ATOMIC_RELAXED) << UNIT_SHIFT;
    s->meta_bytes = __atomic_load_n(&st.meta_bytes, __ATOMIC_RELAXED);
    s->commits = st.commits, s->commit_units = st.commit_units, s->commit_failures = st.commit_failures;
    s->last_error = st.last_error;
    s->faults = st.faults, s->fault_units = st.fault_units, s->fault_retries = st.fault_retries;
    s->fault_refused = st.fault_refused, s->fault_failures = st.fault_failures;
    s->fault_ns = st.fault_ns, s->fault_ns_max = st.fault_ns_max;
    s->decommit_units = st.decommit_units, s->rereserve_fallbacks = st.rereserve_fallbacks;
    s->pin_mode = pin_mode;
    s->pins = st.pins, s->pinned_bytes = __atomic_load_n(&st.pinned_units, __ATOMIC_RELAXED) << UNIT_SHIFT;
    s->pin_backed_bytes = __atomic_load_n(&st.pin_backed_units, __ATOMIC_RELAXED) << UNIT_SHIFT;
    s->pin_failures = st.pin_failures;
    s->pinned_kept_units = st.pinned_kept_units, s->pinned_kept_calls = st.pinned_kept_calls;
    s->zero_mode = __atomic_load_n(&zero_mode, __ATOMIC_RELAXED);
    s->zero_bytes = __atomic_load_n(&st.zero_units, __ATOMIC_RELAXED) << UNIT_SHIFT;
    s->zero_maps = st.zero_maps, s->zero_upgrades = st.zero_upgrades, s->zero_failures = st.zero_failures;
    s->reclaim_mode = __atomic_load_n(&reclaim_mode, __ATOMIC_RELAXED);
    s->reclaim_passes = st.reclaim_passes, s->reclaim_units = st.reclaim_units, s->reclaim_rebacked = st.reclaim_rebacked;
    s->reclaim_flaps = st.reclaim_flaps, s->reclaim_written = st.reclaim_written, s->reclaim_foreign = st.reclaim_foreign;
    s->reclaim_failures = st.reclaim_failures;
}

/* ---- (2026-11-12) cold units on the SSD: the cold thread's part (host/hos_thread.c memory_cold_main; the file's header) ---- */
int hos_ps5_cold_mode(void) {
    const int m = __atomic_load_n(&cold_mode, __ATOMIC_ACQUIRE);
    return m > 0 ? m : HOS_PS5_COLD_OFF;
}
static void cold_file(char *path, size_t cap, const char *what, unsigned n) {
    snprintf(path, cap, "%s/hookshot-cold-%s%02u.bin", ck.dir, what, n);
}
/* address space of the thread's own, reserved once: 0 or the error */
static int cold_space(uint8_t **at, uint64_t bytes) {
    void *m = NULL;
    const int r = ps5_vrange_reserve((size_t)bytes, NULL, UNIT, &m);
    if (r == 0) *at = (uint8_t *)m;
    return r;
}
int hos_ps5_cold_start(char *said, size_t cap) {
    if (cap) said[0] = 0;
    if (__atomic_load_n(&cold_mode, __ATOMIC_ACQUIRE)) return hos_ps5_cold_mode();
    const char *e = getenv("HOOKSHOT_PS5_COLD");
    /* (2026-11-13, the review: "measure", "1" or "on", nothing else. Any other value was the real mode, so that a "mesure" or a
       "Measure" written for the measurement gave the game's units to the SSD: now it leaves the cold units off, and says so) */
    const int asked = !e || !e[0] || !strcmp(e, "0") || !strcmp(e, "off") ? -1 : !strcmp(e, "measure") ? HOS_PS5_COLD_MEASURE
                      : !strcmp(e, "1") || !strcmp(e, "on") ? HOS_PS5_COLD_ON : -2;
    const char *why = NULL;
    if (asked == -1) why = "off (HOOKSHOT_PS5_COLD=measure measures, =1 sets cold units aside on the SSD)";
    else if (asked == -2) why = "OFF: the value is not understood (HOOKSHOT_PS5_COLD=measure measures, =1 or on sets cold units aside on the SSD)";
    else if (!guest_hi) why = "off: no window";
    else if (__atomic_load_n(&pin_mode, __ATOMIC_SEQ_CST) == 0) why = "off: no pins (HOOKSHOT_PS5_GPU_PIN=0): the window does not know what the GPU reaches";
    if (why) {
        snprintf(said, cap, "%s", why);
        __atomic_store_n(&cold_mode, -1, __ATOMIC_RELEASE);
        return HOS_PS5_COLD_OFF;
    }
    const uint64_t window_gibs = (guest_hi - guest_lo) >> GIB_SHIFT;
    ck.lo_gib = env_u64("HOOKSHOT_PS5_COLD_LO_GIB", 256);
    ck.hi_gib = env_u64("HOOKSHOT_PS5_COLD_HI_GIB", 512);
    if (ck.hi_gib > window_gibs) ck.hi_gib = window_gibs;
    if (ck.lo_gib >= ck.hi_gib) ck.lo_gib = ck.hi_gib ? ck.hi_gib - 1 : 0;
    ck.lo = ck.lo_gib * GIB_UNITS, ck.hi = ck.hi_gib * GIB_UNITS;
    ck.park_rate = env_u64("HOOKSHOT_PS5_COLD_PARK_RATE", 2000);
    ck.after_ms = env_u64("HOOKSHOT_PS5_COLD_AFTER_S", 60) * 1000;
    ck.hot_s = env_u64("HOOKSHOT_PS5_COLD_HOT_S", 300);
    ck.hold_s = env_u64("HOOKSHOT_PS5_COLD_HOLD_S", 60);
    if (ck.hot_s > 3600) ck.hot_s = 3600;
    if (ck.hold_s > 3600) ck.hold_s = 3600;
#ifndef HOOKSHOT_PS5_HOST_MODEL
    if (ck.hold_s < 10) ck.hold_s = 10;   /* (a system call follows its commit at once: never less than this on the console) */
#endif
    ck.free_mb = env_u64("HOOKSHOT_PS5_COLD_FREE_MB", 3072);
    ck.write_bps = env_u64("HOOKSHOT_PS5_COLD_WRITE_MB_S", 64) << 20;
    ck.cap_bytes = env_u64("HOOKSHOT_PS5_COLD_CAP_MB", 4096) << 20;
    ck.reserve_units = env_u64("HOOKSHOT_PS5_COLD_RESERVE_MB", 8) * 16;
    if (ck.reserve_units > COLD_RESERVE_MAX) ck.reserve_units = COLD_RESERVE_MAX;
    ck.seg_mb = env_u64("HOOKSHOT_PS5_COLD_SEG_MB", 256);
    if (ck.seg_mb < 1) ck.seg_mb = 1;
    if (ck.cap_bytes > (ck.seg_mb << 20) * COLD_SEGS) ck.cap_bytes = (ck.seg_mb << 20) * COLD_SEGS;
    ck.seg_slots = ck.seg_mb << 20 >> UNIT_SHIFT;
    const char *d = getenv("HOOKSHOT_PS5_COLD_DIR");
    snprintf(ck.dir, sizeof ck.dir, "%s", d && *d ? d : hos_ps5_data_root ? hos_ps5_data_root() : ".");
    if (!cold_base && cold_space(&cold_base, GIBS * COLD_BYTES)) why = "off: no address space for its notes";
    else if (!cold_scratch && cold_space(&cold_scratch, GIBS * UNIT)) why = "off: no address space for its scratch units";
    else if (!cold_staging && cold_space(&cold_staging, COLD_BATCH * UNIT)) why = "off: no address space for its staging units";
    if (!why && asked == HOS_PS5_COLD_MEASURE && !cold_sink) {   /* (the unit the fault handler's timed reads land in) */
        int64_t start = -1;
        uint8_t *at = NULL;
        if (cold_space(&at, UNIT) || allocate(UNIT, &start)) why = "off: no unit for the measured reads";
        else if (map_fixed(at, PROT_RW, start)) sceKernelReleaseDirectMemory(start, UNIT), why = "off: no unit for the measured reads";
        else cold_sink = at;
    }
    if (why) {
        snprintf(said, cap, "%s", why);
        __atomic_store_n(&cold_mode, -1, __ATOMIC_RELEASE);
        return HOS_PS5_COLD_OFF;
    }
    /* what a session before left on the SSD: emptied (the files are this session's alone) */
    char path[220];
    for (unsigned i = 0; i < COLD_SEGS; i++) cold_file(path, sizeof path, "", i), unlink(path);
    for (unsigned i = 0; i < 2; i++) cold_file(path, sizeof path, "probe-", i), unlink(path);
    __atomic_store_n(&cold_clock0, now_ns(), __ATOMIC_RELEASE);   /* (after the settings: a commit reads hold_s once this is set) */
    __atomic_store_n(&cold_mode, asked, __ATOMIC_RELEASE);
    snprintf(said, cap, "%s: GiB %llu to %llu of the window, parked at %llu units a second, cold after %llu s parked, warm for %llu s once "
             "back, %llu s after a commit; to the SSD while the free direct memory is under %llu MiB, %llu MiB/s at most, %llu MiB a session "
             "at most, in %s (files of %llu MiB); %llu units of reserve for the units coming back",
             asked == HOS_PS5_COLD_MEASURE ? "measure (nothing written to the SSD nor given back: \"on the SSD\" is what would be)" : "on",
             (unsigned long long)ck.lo_gib, (unsigned long long)ck.hi_gib, (unsigned long long)ck.park_rate,
             (unsigned long long)(ck.after_ms / 1000), (unsigned long long)ck.hot_s, (unsigned long long)ck.hold_s,
             (unsigned long long)ck.free_mb, (unsigned long long)(ck.write_bps >> 20), (unsigned long long)(ck.cap_bytes >> 20), ck.dir,
             (unsigned long long)ck.seg_mb, (unsigned long long)(asked == HOS_PS5_COLD_ON ? ck.reserve_units : 0));
    return asked;
}

/* the GiB's notes (its lock held), made the first time: every unit held for hold_s, as a commit made before the notes existed
   may have its system call still to come */
static struct cold_note *cold_notes_of(uint64_t g) {
    if (cold_notes[g]) return cold_notes[g];
    if (!cold_base || hos_ps5_cold_mode() == HOS_PS5_COLD_OFF) return NULL;
    int64_t start = -1;
    if (allocate(COLD_BYTES, &start)) return NULL;
    uint8_t *const at = cold_base + g * COLD_BYTES;
    void *placed = at;
    int32_t r = sceKernelMapDirectMemory(&placed, COLD_BYTES, PROT_RW, PS5_KERNEL_MAP_FIXED, start, PS5_KERNEL_PAGE_SIZE);
    if (r == 0 && placed != at) sceKernelMunmap(placed, COLD_BYTES), r = PS5_SHM_NO_PLACE;
    if (r) { sceKernelReleaseDirectMemory(start, COLD_BYTES); return NULL; }
    memset(at, 0, COLD_BYTES);
    struct cold_note *const t = (struct cold_note *)at;
    const uint16_t hold = (uint16_t)(cold_s16() + ck.hold_s + (ck.hold_s != 0));   /* (+1: the clock counts whole seconds) */
    for (uint64_t i = 0; i < GIB_UNITS; i++) t[i].hold = hold;
    ADD(meta_bytes, COLD_BYTES);
    __atomic_store_n(&cold_notes[g], t, __ATOMIC_RELEASE);
    return t;
}

/* Park unit u (its GiB's lock held): 1 parked; 0 not a unit to park; -1 the kernel refused the reservation (the unit is as it
   was, and no more parks this session). Only a unit backed, not pinned (the GPU may reach it), not behind the shared zeros,
   not held (a commit, or back not long ago), not being written, and that the kernel says, for all of it, is plain CPU
   memory (read-write, or read-only for a clean unit) with no GPU access. The state first: from here a fault on the unit,
   which the reservation is about to cause, finds it parked and waits for this lock to map it back */
static int cold_park(uint64_t u, struct cold_note *n, uint16_t now_s) {
    const unsigned s = n->state;
    if (!bit(u) || pin_bit(u) || zero_bit(u) || (s & (C_PARKED | C_SPILLED | C_FOREIGN | C_STAGED)) || before(now_s, n->hold)) return 0;
    int64_t *const table = meta[u / GIB_UNITS];
    if (!table || !table[u % GIB_UNITS]) return 0;
    void *const at = place(u);
    void *qs = NULL, *qe = NULL;
    uint32_t prot = 0;
    const int answered = sceKernelQueryMemoryProtection(at, &qs, &qe, &prot) == 0;
    const uint32_t want = (s & C_CLEAN) ? PS5_KERNEL_PROT_CPU_READ : PROT_RW;
    if (!answered || (uintptr_t)qs > (uintptr_t)at || (uintptr_t)qe < (uintptr_t)at + UNIT || (prot & PROT_RW) != want || (prot & PROT_GPU)) {
        cset(n, s | C_FOREIGN);
        ADD(cold_foreign, 1);
        if (!answered) ADD(cold_unanswered, 1);
        return 0;
    }
    const uint64_t t0 = now_ns();
    n->when = (uint32_t)cold_ms();
    cset(n, (s & ~C_CLEAN) | C_PARKED);   /* (a clean unit keeps its copy: has_copy) */
    set_bit(u, 0);
    ADD(cold_parked, 1);
    if (s & C_CLEAN) __atomic_sub_fetch(&st.cold_clean, 1, __ATOMIC_RELAXED);
    void *placed = at;
    int32_t r = sceKernelReserveVirtualRange(&placed, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
    if (r == 0 && placed != at) sceKernelMunmap(placed, UNIT), r = PS5_SHM_NO_PLACE;
    if (r) {   /* (refused: its memory mapped back over whatever is there, as it was; something the console was never seen to do) */
        const int32_t back = map_fixed(at, want, table[u % GIB_UNITS] - 1);
        set_bit(u, 1);
        cset(n, s);
        __atomic_sub_fetch(&st.cold_parked, 1, __ATOMIC_RELAXED);
        if (s & C_CLEAN) ADD(cold_clean, 1);
        ADD(cold_park_refused, 1);
        __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
        if (back) cold_fatal(u, "a park refused, and the unit's memory could not be mapped back", back);
        return -1;
    }
    const uint64_t ns = now_ns() - t0;
    ADD(cold_parks, 1);
    ADD(cold_park_ns, ns);
    max_of(&st.cold_park_ns_max, ns);
    return 1;
}
/* park up to `budget` units, from where the last call stopped; the parks made */
static uint64_t cold_park_sweep(uint64_t budget) {
    uint64_t made = 0, looked = 0, u = ck.park_cursor;
    const uint16_t now_s = cold_s16();
    while (made < budget && looked < 65536 && !ck.parks_stopped) {
        if (u < ck.lo || u >= ck.hi) u = ck.lo;
        const uint64_t g = u / GIB_UNITS;
        if (!__atomic_load_n(&meta[g], __ATOMIC_ACQUIRE)) { u = (g + 1) * GIB_UNITS; looked += 64; continue; }   /* (never backed) */
        if (!(u & 7) && !__atomic_load_n(&backed[u >> 3], __ATOMIC_RELAXED)) { u += 8; looked++; continue; }
        looked++;
        struct cold_note *n = cnote(u);
        if (!bit(u) || pin_bit(u) || zero_bit(u) ||
            (n && ((cstate(n) & (C_PARKED | C_SPILLED | C_FOREIGN | C_STAGED)) || before(now_s, __atomic_load_n(&n->hold, __ATOMIC_RELAXED))))) {
            u++;
            continue;
        }
        lock_gib(g);
        if (!n && cold_notes_of(g)) n = cnote(u);   /* (the GiB's first: all its units held for hold_s, so none is parked yet) */
        const int r = n ? cold_park(u, n, now_s) : 0;
        unlock_gib(g);
        if (r < 0) {
            ck.parks_stopped = 1, ck.stop_why = COLD_STOP_PARK_REFUSED;
            break;
        }
        made += (uint64_t)r;
        u++;
    }
    ck.park_cursor = u;
    return made;
}

/* the writer's: one unit staged (mapped read-only at its staging address, its lock held while it is, then not) */
struct staged {
    uint64_t u;
    int64_t start;
    uint32_t sum, slot;   /* slot + 1 once written */
    uint8_t gen, zero;
};
static int32_t write_all(int fd, const uint8_t *p, size_t n, off_t off) {
    while (n) {
        const ssize_t w = pwrite(fd, p, n, off);
        if (w > 0) p += w, n -= (size_t)w, off += w;
        else if (w < 0 && errno == EINTR) continue;
        else return w < 0 ? errno : EIO;
    }
    return 0;
}
static int cold_seg_fd(uint64_t seg) {
    const int fd = __atomic_load_n(&cold_fd[seg], __ATOMIC_ACQUIRE) - 1;
    if (fd >= 0) return fd;
    char path[220];
    cold_file(path, sizeof path, "", (unsigned)seg);
    const int made = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (made < 0) return -1;
    __atomic_store_n(&cold_fd[seg], made + 1, __ATOMIC_RELEASE);
    return made;
}
/* a unit parked with its copy still its bytes, or the batch's units once written: on the SSD now (its lock held, the unit
   parked and not moved since). Its memory given back (measure: kept) */
static void cold_spill_unit(uint64_t u, struct cold_note *n, int64_t start, unsigned extra) {
    const unsigned s = n->state;
    if (cold_mode == HOS_PS5_COLD_ON) {
        sceKernelReleaseDirectMemory(start, UNIT);
        meta[u / GIB_UNITS][u % GIB_UNITS] = 0;
        __atomic_sub_fetch(&st.backed_units, 1, __ATOMIC_RELAXED);
    }
    const unsigned next = (s & ~(C_PARKED | C_STAGED)) | C_SPILLED | extra;
    cset(n, next);
    __atomic_sub_fetch(&st.cold_parked, 1, __ATOMIC_RELAXED);
    ADD(cold_spilled, 1);
    if (next & C_ZERO) ADD(cold_spilled_zero, 1);
    ADD(cold_spills, 1);
}
/* the batch: sums, which are zeros, the writes (16 units at most, run by run of units that need one, within a file), the
   staging units back to a reservation, then each unit under its lock: on the SSD if it is still parked and did not move,
   else left as it is (written for nothing). Returns the bytes written */
static uint64_t cold_write_batch(struct staged *b, unsigned k) {
    uint64_t wrote = 0;
    uint64_t units[COLD_BATCH];
    for (unsigned i = 0; i < k; i++) {
        const uint8_t *const p = cold_staging + (uint64_t)i * UNIT;
        b[i].zero = (uint8_t)all_zero(p);
        b[i].sum = cold_sum(p);
        b[i].slot = 0;
        units[i] = (uint64_t)(uintptr_t)place(b[i].u);   /* (the hook's: where they are) */
    }
    WRITER_HOOK(0, units, k);
    for (unsigned i = 0; i < k && !ck.writes_stopped;) {
        if (b[i].zero) { i++; continue; }
        const uint64_t first = ck.slots_used, seg = first / ck.seg_slots;
        unsigned j = i + 1;
        while (j < k && !b[j].zero && (first + (j - i)) / ck.seg_slots == seg) j++;
        const uint64_t bytes = (uint64_t)(j - i) << UNIT_SHIFT;
        if (__atomic_load_n(&st.cold_written, __ATOMIC_RELAXED) + bytes > ck.cap_bytes || seg >= COLD_SEGS) {
            ck.writes_stopped = 1, ck.stop_why = COLD_STOP_CAP;
            break;
        }
        if (cold_mode == HOS_PS5_COLD_ON) {
            const int fd = cold_seg_fd(seg);
            if (fd < 0) {   /* (no file: no more writes, and no more parks, which would cost faults for nothing) */
                ck.writes_stopped = ck.parks_stopped = 1, ck.stop_why = COLD_STOP_OPEN;
                __atomic_store_n(&st.last_error, errno, __ATOMIC_RELAXED);
                break;
            }
            const uint64_t t0 = now_ns();
            const int32_t r = write_all(fd, cold_staging + (uint64_t)i * UNIT, (size_t)bytes, (off_t)((first % ck.seg_slots) << UNIT_SHIFT));
            const uint64_t ns = now_ns() - t0;
            ADD(cold_writes, 1);
            ADD(cold_write_ns, ns);
            max_of(&st.cold_write_ns_max, ns);
            if (r) {   /* (a full SSD, an error: these units stay parked, their memory theirs; no more writes) */
                ADD(cold_write_errors, 1);
                __atomic_store_n(&st.last_error, r, __ATOMIC_RELAXED);
                ck.writes_stopped = ck.parks_stopped = 1, ck.stop_why = COLD_STOP_WRITE;
                break;
            }
        } else ADD(cold_writes, 1);
        for (unsigned x = i; x < j; x++) b[x].slot = (uint32_t)(first + (x - i) + 1);
        ck.slots_used += j - i;
        ADD(cold_written, bytes);
        wrote += bytes;
        i = j;
    }
    WRITER_HOOK(1, units, k);
    /* (nothing maps the units' memory now but, for those back, their place. 2026-11-13: the reservation refused, the staging
       address is lost: it is never mapped over again, no park nor write follows, and no unit of the batch goes to the SSD, its
       memory kept as a parked unit's) */
    const int lost = lay_reservation(cold_staging, (size_t)k * UNIT) != 0;
    if (lost) space_lost(), ck.writes_stopped = ck.parks_stopped = 1, ck.stop_why = COLD_STOP_SPACE;
    for (unsigned i = 0; i < k; i++) {
        const uint64_t u = b[i].u, g = u / GIB_UNITS;
        lock_gib(g);
        struct cold_note *const n = cnote(u);
        const unsigned s = n->state;
        if (s & C_DEAD) {   /* (decommitted while staged: its memory is the writer's to give back) */
            sceKernelReleaseDirectMemory(b[i].start, UNIT);
            cset(n, s & ~(C_STAGED | C_DEAD));
            ADD(cold_raced, 1);
        } else if (!lost && (s & C_PARKED) && n->gen == b[i].gen && (b[i].zero || b[i].slot)) {
            n->slot = b[i].zero ? 0 : b[i].slot;
            n->sum = b[i].sum;
            cold_spill_unit(u, n, b[i].start, b[i].zero ? C_ZERO : 0);
            if (b[i].zero) ADD(cold_spills_zero, 1);
            else ADD(cold_spills_written, 1);
        } else {   /* (back since it was staged, or not written: as it is) */
            cset(n, s & ~C_STAGED);
            if (b[i].slot) ADD(cold_raced, 1);
        }
        unlock_gib(g);
    }
    return wrote;
}
/* units parked for after_ms and more to the SSD: up to `want` of them given back, `budget` bytes written. Returns the bytes */
static uint64_t cold_spill(uint64_t budget, uint64_t want) {
    struct staged b[COLD_BATCH];
    unsigned k = 0;
    uint64_t wrote = 0, gave = 0, looked = 0, u = ck.spill_cursor;
    const uint32_t now = (uint32_t)cold_ms();
    while (gave < want && looked < 32768 && !__atomic_load_n(&cold_space_lost, __ATOMIC_ACQUIRE)) {   /* (2026-11-13: none once an address is lost) */
        if (u < ck.lo || u >= ck.hi) u = ck.lo;
        const uint64_t g = u / GIB_UNITS;
        if (!__atomic_load_n(&cold_notes[g], __ATOMIC_ACQUIRE)) { u = (g + 1) * GIB_UNITS; looked += 64; continue; }
        looked++;
        struct cold_note *const n = cnote(u);
        const unsigned hint = cstate(n);
        if ((hint & (C_PARKED | C_STAGED | C_SPILLED)) != C_PARKED || (uint32_t)(now - n->when) < ck.after_ms) { u++; continue; }
        const int copy = has_copy(n);
        if (!copy && (ck.writes_stopped || wrote + ((uint64_t)(k + 1) << UNIT_SHIFT) > budget)) { u++; continue; }
        lock_gib(g);
        const unsigned s = n->state;
        int64_t *const table = meta[g];
        if ((s & (C_PARKED | C_STAGED | C_SPILLED)) == C_PARKED && (uint32_t)(now - n->when) >= ck.after_ms && table[u % GIB_UNITS] &&
            !cold_scratch_lost[g]) {   /* (2026-11-13: a GiB whose scratch is lost sends nothing more: it could not come back) */
            const int64_t start = table[u % GIB_UNITS] - 1;
            if (has_copy(n)) {   /* (back read-only since it was last written, and parked again: its copy is its bytes) */
                cold_spill_unit(u, n, start, 0);
                ADD(cold_spills_again, 1);
                gave++;
            } else if (map_fixed(cold_staging + (uint64_t)k * UNIT, PS5_KERNEL_PROT_CPU_READ, start) == 0) {
                cset(n, s | C_STAGED);
                b[k].u = u, b[k].start = start, b[k].gen = n->gen;
                k++;
                ADD(cold_staged, 1);
            }
        }
        unlock_gib(g);
        u++;
        if (k == COLD_BATCH) {
            const uint64_t w = cold_write_batch(b, k);
            wrote += w, gave += k, k = 0;   /* (counted as given: a few that were not only end the call a batch early) */
            if (wrote >= budget || ck.writes_stopped) break;
        }
    }
    if (k) wrote += cold_write_batch(b, k);
    ck.spill_cursor = u;
    return wrote;
}
static void reserve_fill(void) {
    while (__atomic_load_n(&cold_reserve_n, __ATOMIC_RELAXED) < ck.reserve_units) {
        int64_t start = -1;
        if (allocate(UNIT, &start)) return;
        reserve_take();
        cold_reserve[cold_reserve_n++] = start;
        reserve_give();
    }
}
/* One step of the cold thread, every 100 ms or so: calm, the game shows frames and its window does not grow (host/hos_thread.c
   says); free_mb, the free direct memory as last measured. Parks at the rate while calm; sends cold units to the SSD while
   calm and the free memory is under HOOKSHOT_PS5_COLD_FREE_MB (measure: the free memory plus what is "on the SSD"), and, when
   on, the reserve full */
void hos_ps5_cold_tick(int calm, uint64_t free_mb) {
    const int mode = hos_ps5_cold_mode();
    if (!mode || __atomic_load_n(&cold_failed, __ATOMIC_ACQUIRE)) return;
    const uint64_t now = now_ns(), dt = ck.last_ns ? now - ck.last_ns : 0;
    ck.last_ns = now;
    ADD(cold_ticks, 1);
    const int pins = __atomic_load_n(&pin_mode, __ATOMIC_SEQ_CST);
    if (pins == 0) {   /* (pins off: the window does not know what the GPU reaches; what is away comes back as it is touched) */
        ck.parks_stopped = ck.writes_stopped = 1, ck.stop_why = COLD_STOP_PINS;
        return;
    }
    if (pins < 0) return;   /* (no import asked yet: the setting not read) */
    if (__atomic_load_n(&cold_space_lost, __ATOMIC_ACQUIRE)) {   /* (2026-11-13: an address of its own lost, the fault handler's or the writer's) */
        if (!ck.parks_stopped || !ck.writes_stopped) ck.parks_stopped = ck.writes_stopped = 1, ck.stop_why = COLD_STOP_SPACE;
        return;
    }
    if (mode == HOS_PS5_COLD_ON) reserve_fill();
    if (!calm) {
        ck.park_credit = ck.write_credit = 0;
        return;
    }
    ADD(cold_calm, 1);
    const uint64_t step = dt > 1000000000ull ? 1000000000ull : dt;
    if (!ck.parks_stopped) {
        ck.park_credit += ck.park_rate * step / 1000000000ull;
        if (ck.park_credit > ck.park_rate) ck.park_credit = ck.park_rate;
        const uint64_t made = cold_park_sweep(ck.park_credit);
        ck.park_credit = made >= ck.park_credit ? 0 : ck.park_credit - made;
    }
    const uint64_t away_mb = mode == HOS_PS5_COLD_MEASURE ? __atomic_load_n(&st.cold_spilled, __ATOMIC_RELAXED) >> 4 : 0;
    if (free_mb + away_mb >= ck.free_mb) return;
    if (mode == HOS_PS5_COLD_ON && __atomic_load_n(&cold_reserve_n, __ATOMIC_RELAXED) < ck.reserve_units) return;
    uint64_t want = (ck.free_mb - free_mb - away_mb) << 4;
    if (mode == HOS_PS5_COLD_ON && hos_ps5_memory_pieces) {   /* (each unit given back may be a free piece more: under COLD_PIECES_MAX) */
        uint64_t no_walk = 0;
        ck.pieces = hos_ps5_memory_pieces(&no_walk);
        if (no_walk != ck.no_walk_seen || ck.pieces + COLD_BATCH >= COLD_PIECES_MAX) {
            ck.no_walk_seen = no_walk;
            ck.crowded++;
            return;
        }
        if (want > COLD_PIECES_MAX - ck.pieces) want = COLD_PIECES_MAX - ck.pieces;
    }
    ck.write_credit += ck.write_bps * step / 1000000000ull;
    if (ck.write_credit > ck.write_bps) ck.write_credit = ck.write_bps;
    const uint64_t wrote = cold_spill(ck.write_credit, want);
    ck.write_credit = wrote >= ck.write_credit ? 0 : ck.write_credit - wrote;
}

/* ---- measure: the SSD probe, once (the cold thread's): writes and reads of 64 KiB and 1 MiB in a file of its own, reads while
   another thread writes that file or another; the file is kept for the fault handler's timed reads ---- */
struct probe_time { uint64_t n, sum, max, min; };
static void probe_add(struct probe_time *t, uint64_t ns) {
    t->n++, t->sum += ns;
    if (ns > t->max) t->max = ns;
    if (!t->min || ns < t->min) t->min = ns;
}
struct probe_writer { int fd; const uint8_t *buf; uint64_t at; unsigned n; struct probe_time t; int32_t error; };
static void *probe_writer_main(void *p) {
    struct probe_writer *const w = (struct probe_writer *)p;
    for (unsigned i = 0; i < w->n && !w->error; i++) {
        const uint64_t t0 = now_ns();
        w->error = write_all(w->fd, w->buf, (size_t)COLD_BATCH * UNIT, (off_t)(w->at + ((uint64_t)i << 20)));
        probe_add(&w->t, now_ns() - t0);
    }
    return NULL;
}
static int32_t probe_reads(int fd, uint8_t *to, uint64_t slots, unsigned n, unsigned stride, struct probe_time *t) {
    for (unsigned i = 0; i < n; i++) {
        const uint64_t t0 = now_ns();
        const ssize_t got = pread(fd, to, UNIT, (off_t)((((uint64_t)i * stride + 3) % slots) << UNIT_SHIFT));
        if (got != (ssize_t)UNIT) return got < 0 ? errno : EIO;
        probe_add(t, now_ns() - t0);
    }
    return 0;
}
static int probe_say(char *buf, size_t cap, const char *what, const struct probe_time *t, int ms) {
    const double div = ms ? 1e6 : 1e3;
    return snprintf(buf, cap, "%s %.*f/%.*f/%.*f %s (%llu)", what, ms ? 1 : 0, t->n ? t->min / div : 0, ms ? 1 : 0,
                    t->n ? (double)t->sum / (double)t->n / div : 0, ms ? 1 : 0, t->max / div, ms ? "ms" : "us", (unsigned long long)t->n);
}
int hos_ps5_cold_probe(char *buf, size_t cap) {
    if (cap) buf[0] = 0;
    if (hos_ps5_cold_mode() != HOS_PS5_COLD_MEASURE || __atomic_load_n(&cold_probe_fd, __ATOMIC_ACQUIRE) >= 0) return 0;
    if (__atomic_load_n(&cold_space_lost, __ATOMIC_ACQUIRE)) {   /* (2026-11-13: the staging address may be lost: not mapped over) */
        if (cap) snprintf(buf, cap, "the SSD probe: not run, an address of the cold thread's own was lost");
        return 1;
    }
    /* (1 MiB of direct memory at the staging address, 64 KiB at a time: the writer is this thread, between its steps) */
    int64_t starts[COLD_BATCH];
    unsigned have = 0;
    for (; have < COLD_BATCH; have++) {
        if (allocate(UNIT, &starts[have])) break;
        if (map_fixed(cold_staging + (uint64_t)have * UNIT, PROT_RW, starts[have])) {   /* (2026-11-13: not kept, then) */
            sceKernelReleaseDirectMemory(starts[have], UNIT);
            break;
        }
    }
    char path[220], other[220];
    cold_file(path, sizeof path, "probe-", 0);
    cold_file(other, sizeof other, "probe-", 1);
    int32_t err = have < COLD_BATCH ? -1 : 0;
    const char *where = have < COLD_BATCH ? "no memory for its buffer" : NULL;
    uint8_t *const mem = cold_staging;
    struct probe_time w64 = {0}, w1m = {0}, rb = {0}, rd = {0}, rsame = {0}, rother = {0};
    struct probe_writer same = {0}, oth = {0};
    int fd = -1, fdd = -1, fd2 = -1, direct = 0;
    if (!err) {
        for (uint64_t i = 0; i < COLD_BATCH * UNIT / 8; i++) ((uint64_t *)mem)[i] = i * 0x9e3779b97f4a7c15ull;
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) err = errno, where = "the probe's file could not be made";
    }
    for (unsigned i = 0; !err && i < 32; i++) {   /* 32 writes of 64 KiB, then 16 of 1 MiB: 18 MiB */
        const uint64_t t0 = now_ns();
        if ((err = write_all(fd, mem, UNIT, (off_t)((uint64_t)i << UNIT_SHIFT))) != 0) where = "a 64 KiB write";
        else probe_add(&w64, now_ns() - t0);
    }
    for (unsigned i = 0; !err && i < 16; i++) {
        const uint64_t t0 = now_ns();
        if ((err = write_all(fd, mem, (size_t)COLD_BATCH * UNIT, (off_t)(((uint64_t)2 << 20) + ((uint64_t)i << 20)))) != 0) where = "a 1 MiB write";
        else probe_add(&w1m, now_ns() - t0);
    }
    const uint64_t slots = 18 * 16;
    if (!err && (err = probe_reads(fd, mem, slots, 64, 37, &rb)) != 0) where = "a 64 KiB read";
    if (!err) {   /* (O_DIRECT: what the SSD itself answers, the console's cache aside; refused: buffered) */
        fdd = open(path, O_RDONLY | O_DIRECT);
        direct = fdd >= 0;
        if (!direct) fdd = open(path, O_RDONLY);
        if (fdd < 0) err = errno, where = "the probe's file could not be opened again";
        else if ((err = probe_reads(fdd, mem, slots, 64, 53, &rd)) != 0) where = "a 64 KiB direct read";
    }
    if (!err) {   /* reads while another thread writes 8 MiB at the end of the same file, then of another */
        pthread_t t;
        same = (struct probe_writer){fd, mem, (uint64_t)18 << 20, 8, {0}, 0};
        if (pthread_create(&t, NULL, probe_writer_main, &same) == 0) {
            err = probe_reads(fdd, mem + 8 * UNIT, slots, 64, 41, &rsame);
            pthread_join(t, NULL);
            if (err) where = "a read beside a write of the same file";
            else if (same.error) err = same.error, where = "a 1 MiB write beside reads";
        }
        fd2 = err ? -1 : open(other, O_RDWR | O_CREAT | O_TRUNC, 0666);
        oth = (struct probe_writer){fd2, mem, 0, 8, {0}, 0};
        if (fd2 >= 0 && pthread_create(&t, NULL, probe_writer_main, &oth) == 0) {
            err = probe_reads(fdd, mem + 8 * UNIT, slots, 64, 43, &rother);
            pthread_join(t, NULL);
            if (err) where = "a read beside a write of another file";
        }
        if (fd2 >= 0) close(fd2);
        unlink(other);
    }
    if (fdd >= 0) close(fdd);
    if (fd >= 0) close(fd);
    /* (2026-11-13, the review: the staging address reserved again before its memory goes back, as the writer does. The other way
       round, a release that unmaps what it gives back leaves the range free for a moment, and the reservation laid over it then
       (MAP_FIXED) could take a mapping another thread had just been given there. Refused: the address is lost, as the writer's) */
    if (lay_reservation(cold_staging, (size_t)COLD_BATCH * UNIT)) space_lost();
    for (unsigned i = 0; i < have; i++) sceKernelReleaseDirectMemory(starts[i], UNIT);
    if (!err) {   /* (kept, read by the fault handler: 26 MiB) */
        const int r = open(path, O_RDONLY);
        if (r >= 0) {
            cold_probe_slots = 26 * 16;
            __atomic_store_n(&cold_probe_fd, r, __ATOMIC_RELEASE);
        }
    }
    size_t at = 0;
    at += (size_t)snprintf(buf + at, cap - at, "the SSD probe (%s, min/average/max): ", path);
    at += (size_t)probe_say(buf + at, cap - at, "64 KiB writes", &w64, 1);
    at += (size_t)probe_say(buf + at, cap - at, "; 1 MiB writes", &w1m, 1);
    at += (size_t)probe_say(buf + at, cap - at, "; 64 KiB reads, buffered", &rb, 0);
    at += (size_t)probe_say(buf + at, cap - at, direct ? "; O_DIRECT" : "; again (O_DIRECT refused)", &rd, 0);
    at += (size_t)probe_say(buf + at, cap - at, "; while the same file is written", &rsame, 0);
    at += (size_t)probe_say(buf + at, cap - at, " (its 1 MiB writes", &same.t, 1);
    at += (size_t)probe_say(buf + at, cap - at, "); while another file is written", &rother, 0);
    at += (size_t)probe_say(buf + at, cap - at, " (its 1 MiB writes", &oth.t, 1);
    if (at < cap) snprintf(buf + at, cap - at, "); %s%s%s, error %d; 34 MiB written at most", err ? "FAILED at " : "done",
                           err && where ? where : "", "", (int)err);
    return 1;
}

void hos_ps5_cold_stats(struct hos_ps5_cold *c) {
    memset(c, 0, sizeof *c);
    c->mode = hos_ps5_cold_mode();
    c->stop_why = ck.stop_why, c->parks_stopped = ck.parks_stopped, c->writes_stopped = ck.writes_stopped;
    c->last_error = st.last_error;
#define C(f) c->f = __atomic_load_n(&st.cold_##f, __ATOMIC_RELAXED)
    C(parked), C(spilled), C(spilled_zero), C(clean), C(parks), C(park_ns), C(park_ns_max), C(park_refused), C(foreign), C(unanswered);
    C(unparks), C(unpark_ns), C(unpark_ns_max), C(spills), C(spills_written), C(spills_again), C(spills_zero), C(raced), C(staged);
    C(written), C(writes), C(write_ns), C(write_ns_max), C(write_errors), C(reloads), C(reload_ns), C(reload_ns_max), C(read_ns);
    C(read_ns_max), C(reload_peak), C(from_reserve), C(sum_mismatch), C(probe_reads), C(probe_read_errors), C(dirtied), C(stale);
    C(released), C(ticks), C(calm);
#undef C
    for (int i = 0; i < 4; i++) c->unpark_age[i] = __atomic_load_n(&st.cold_unpark_age[i], __ATOMIC_RELAXED);
    c->cap_bytes = ck.cap_bytes;
    c->reserve_now = __atomic_load_n(&cold_reserve_n, __ATOMIC_RELAXED);
    c->space_lost = __atomic_load_n(&cold_space_lost, __ATOMIC_ACQUIRE);
    c->slots_used = ck.slots_used;
}
/* (measure) the pieces of direct memory the units "on the SSD" would have given back: runs of adjacent 64 KiB, as their starts
   are read now without the locks (a count, not a snapshot). Each run is one free piece at most once given back, fewer when it
   joins free memory beside it: what COLD_PIECES_MAX would allow of them */
static uint64_t cold_runs_map[((uint64_t)16 << 30 >> UNIT_SHIFT) / 64];
static uint64_t cold_measure_runs(uint64_t *units) {
    memset(cold_runs_map, 0, sizeof cold_runs_map);
    uint64_t n = 0, runs = 0;
    for (uint64_t g = ck.lo_gib; g < ck.hi_gib; g++) {
        const struct cold_note *const t = __atomic_load_n(&cold_notes[g], __ATOMIC_ACQUIRE);
        const int64_t *const table = __atomic_load_n(&meta[g], __ATOMIC_ACQUIRE);
        if (!t || !table) continue;
        for (uint64_t i = 0; i < GIB_UNITS; i++) {
            if (!(__atomic_load_n(&t[i].state, __ATOMIC_RELAXED) & C_SPILLED)) continue;
            const int64_t e = __atomic_load_n(&table[i], __ATOMIC_RELAXED);
            const uint64_t k = e > 0 ? (uint64_t)(e - 1) >> UNIT_SHIFT : UINT64_MAX;
            if (k >= sizeof cold_runs_map * 8) continue;
            cold_runs_map[k / 64] |= (uint64_t)1 << (k % 64);
            n++;
        }
    }
    for (uint64_t k = 0; k < sizeof cold_runs_map * 8; k++)
        if ((cold_runs_map[k / 64] >> (k % 64) & 1) && (k == 0 || !(cold_runs_map[(k - 1) / 64] >> ((k - 1) % 64) & 1))) runs++;
    *units = n;
    return runs;
}
/* the line: every 30 s from the cold thread, and at the memory floor's stop */
void hos_ps5_cold_line(char *buf, size_t cap) {
    struct hos_ps5_cold c;
    hos_ps5_cold_stats(&c);
    if (cap) buf[0] = 0;
    if (!c.mode) return;
    static const char *const why[] = {"", "; PARKS STOPPED: the kernel refused one", "; WRITES AND PARKS STOPPED: a file could not be made",
                                      "; WRITES AND PARKS STOPPED: a write failed (the SSD full?)", "; writes stopped: the session's cap reached",
                                      "; STOPPED: pins off",
                                      "; WRITES AND PARKS STOPPED: an address of its own LOST (the kernel did not lay its reservation again)"};
    const double per = c.reloads ? 1e3 * (double)c.reloads : 1;
    snprintf(buf, cap,
             "%s: %llu MiB on the SSD now (%llu units, %llu of them zeros: nothing written), %llu MiB parked (their memory held), %llu MiB "
             "read-only with their copy; %s %llu MiB of %llu (%llu writes, %.1f ms average, %.1f the longest), read back %llu MiB (%llu units: "
             "%.0f us a unit in the fault handler on average, %.0f the longest; the read alone %.0f/%.0f us; %llu in the busiest second; "
             "%llu from the reserve, %llu in it), back to the SSD without a write %llu MiB, copies dropped by a write %llu, written for "
             "nothing %llu; came back from parking %llu (under 1 s %llu, 1-10 s %llu, 10-60 s %llu, over 60 s %llu; %.0f us on average); "
             "%llu parks (%.0f us, the longest %.0f), %llu units not the CPU's alone (%llu unanswered), %llu decommitted while away; calm "
             "%llu%% of %llu ticks%s",
             c.mode == HOS_PS5_COLD_MEASURE ? "measure" : "on", (unsigned long long)(c.spilled >> 4), (unsigned long long)c.spilled,
             (unsigned long long)c.spilled_zero, (unsigned long long)(c.parked >> 4), (unsigned long long)(c.clean >> 4),
             c.mode == HOS_PS5_COLD_MEASURE ? "would have written" : "written", (unsigned long long)(c.written >> 20),
             (unsigned long long)(c.cap_bytes >> 20), (unsigned long long)c.writes, c.writes ? c.write_ns / 1e6 / (double)c.writes : 0.0,
             c.write_ns_max / 1e6, (unsigned long long)(c.reloads >> 4), (unsigned long long)c.reloads, (double)c.reload_ns / per,
             c.reload_ns_max / 1e3, (double)c.read_ns / per, c.read_ns_max / 1e3, (unsigned long long)c.reload_peak,
             (unsigned long long)c.from_reserve, (unsigned long long)c.reserve_now, (unsigned long long)(c.spills_again >> 4),
             (unsigned long long)c.dirtied, (unsigned long long)c.raced, (unsigned long long)c.unparks,
             (unsigned long long)c.unpark_age[0], (unsigned long long)c.unpark_age[1], (unsigned long long)c.unpark_age[2],
             (unsigned long long)c.unpark_age[3], c.unparks ? c.unpark_ns / 1e3 / (double)c.unparks : 0.0, (unsigned long long)c.parks,
             c.parks ? c.park_ns / 1e3 / (double)c.parks : 0.0, c.park_ns_max / 1e3, (unsigned long long)c.foreign,
             (unsigned long long)c.unanswered, (unsigned long long)c.released,
             (unsigned long long)(c.ticks ? 100 * c.calm / c.ticks : 0), (unsigned long long)c.ticks,
             c.stop_why > 0 && c.stop_why < 7 ? why[c.stop_why] : "");
    const size_t at = strlen(buf);
    if (c.mode == HOS_PS5_COLD_MEASURE) {
        uint64_t units = 0;
        const uint64_t runs = cold_measure_runs(&units);
        if (at < cap)
            snprintf(buf + at, cap - at, "; measure: %llu sums that differ when \"read back\" (must be 0), %llu timed reads of the probe's file "
                     "in the fault handler (%llu failed); the memory of the %llu units \"on the SSD\" lies in %llu runs of adjacent direct "
                     "memory (free pieces it would make at most; the giving back stops at %d pieces)", (unsigned long long)c.sum_mismatch,
                     (unsigned long long)c.probe_reads, (unsigned long long)c.probe_read_errors, (unsigned long long)units,
                     (unsigned long long)runs, COLD_PIECES_MAX);
    } else if (at < cap)
        snprintf(buf + at, cap - at, "; %llu free pieces of direct memory at the last step, %llu steps that sent nothing for them (at most %d)",
                 (unsigned long long)ck.pieces, (unsigned long long)ck.crowded, COLD_PIECES_MAX);
}
#ifdef HOOKSHOT_PS5_HOST_MODEL
/* (the host test's: a unit parked now, by the sweep's own rule (its holds too), under its lock; 1 parked) */
int hos_ps5_cold_park_now(void *p) {
    const uint64_t u = ((uint64_t)(uintptr_t)p - guest_lo) >> UNIT_SHIFT, g = u / GIB_UNITS;
    lock_gib(g);
    struct cold_note *const n = cold_notes_of(g) ? cnote(u) : NULL;
    const int r = n ? cold_park(u, n, cold_s16()) : 0;
    unlock_gib(g);
    return r;
}
/* its cold state bits (C_*), and its slot */
unsigned hos_ps5_cold_state(const void *p, uint32_t *slot) {
    const uint64_t u = ((uint64_t)(uintptr_t)p - guest_lo) >> UNIT_SHIFT;
    struct cold_note *const n = cnote(u);
    if (slot) *slot = n ? n->slot : 0;
    return cstate(n);
}
#endif
#endif /* __PROSPERO__ || HOOKSHOT_PS5_HOST_MODEL */
