// (ps5) The GPU trail: what the renderer gave the GPU, kept in memory, written out when the GPU stops answering.
//
// (2026-11-10: on the console the GPU sometimes stops executing. A submission never completes, the thread that presents stays
// inside the driver's submit or in a wait, and nothing says why: no device lost, no fault address. Five runs froze that way
// before the cause was found by elimination. What was missing each time is a record of what the GPU had been given.)
//
// This header is the instrument's rules and its memory, with no Vulkan and no system call in it, so that tests/test_gpu_trail.py
// compiles and runs it alone. vk_gputrail.cpp turns it on (RX_VK_GPU_TRAIL), gives it a clock and writes its dump out.
//
// What is kept:
// - a ring of kEntries entries, one a submit: a recording handed to the queue (E_REC: held for a batch, or submitted by itself),
//   a batch sent (E_FLUSH: the vkQueueSubmit2 that carries the held recordings), a present's own submit, a read-back's, a
//   sparse bind, a parallel replay's segment. Each has its numbers on the renderer's timelines, its command buffers, its
//   thread and its time, and whether the driver's call made for it came back;
// - with each entry, what went into the recordings since the entry before: the program keys drawn and dispatched, each once
//   with its counts (a ring of kKeyRing keys beside the entries: an entry has 0 to kAccKeys of them), and the other commands
//   folded by kind, image dimension and format (a ring of kDetRing details);
// - what the run did for the first time: every kind of command by dimension (and by format, for the clears) with its first
//   and last use and its count, never dropped; and the last kNewProgs programs used for the first time.
//
// Who writes: a draw or a dispatch goes into its recording's own Acc (one a recording slot, written by the one thread that
// records the slot: no lock; the same key as the use before is two increments), found by the command buffer it is recorded
// into, not through a thread-local: on the console every thread-local is a call (__emutls_get_address). Everything else is
// rare (a few to a few hundred a frame) and takes the trail's spinlock, which is held for a table look-up or one entry's
// copy, never across a call into the driver, the system or the log. A dump formats its text under that lock (well under a
// millisecond) and is written out after it is let go.
//
// SPDX-License-Identifier: GPL-3.0-or-later (hookshot-ps5)
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mkwvk::gpu_trail {

constexpr uint32_t kEntries = 1024;   // submits kept
constexpr uint32_t kKeyRing = 16384;  // their program keys
constexpr uint32_t kDetRing = 8192;   // their other commands, folded
constexpr int kSlots = 64;            // recordings open at once (vk_replay.cpp's kRecordings is 48)
constexpr int kCmds = 256;            // command buffers known by their recording (a slot's is its own for the run)
constexpr int kAccKeys = 128;         // distinct programs kept of one recording (a part is 120 draws and dispatches: RX_VK_CHUNK)
constexpr int kAccHash = 256;
constexpr int kAccDet = 24;           // distinct (kind, dimension, format) kept of one recording
constexpr uint32_t kSeen = 16384;     // programs seen in the run (first uses are flagged while it is under 3/4 full)
constexpr int kWhats = 192;           // kinds of work seen in the run
constexpr int kNewProgs = 48;         // the last programs used for the first time
constexpr int kLine = 900;            // a dump line's text (the trail's line holds 1,024 bytes with its stamp and the dump's prefix)
// (a dump is as many lines of the stage trail, each synced to the console's disk, tens of milliseconds a line: what it shows is
// counted. A batch is 32 command buffers at most: 32 recordings and their flush)
constexpr int kBefore = 8;            // completed entries a dump shows before the first that is not
constexpr int kFirstBad = 34;         // entries it shows from the first that is not: its whole batch
constexpr int kTail = 8;              // and the newest ones, when there are more than that after it
constexpr int kWaits = 16;            // waits for a submission watched at once

// ---- a program's uses in one recording ----
enum : uint8_t {
    K_DRAW = 1,          // a draw
    K_MESH = 2,          // a draw of a task/mesh program run as mesh shaders
    K_FALLBACK = 4,      // a mesh draw through the compute fallback: its vertex draw
    K_DISPATCH = 8,      // a compute dispatch
    K_FB_DISPATCH = 16,  // the fallback's compute dispatches (the task and mesh stages)
    K_INDIRECT = 32,     // a dispatch whose workgroups the GPU reads (those kept are what guest memory held when it was recorded)
    K_FIRST = 64,        // the program's first use in the run
};
struct KeyRec {
    uint64_t key;
    uint32_t groups;        // its dispatches' workgroups, summed (saturating)
    uint16_t draws, disp;   // (saturating)
    uint16_t x, y, z;       // its largest dispatch
    uint8_t flags, spare;
};
static_assert(sizeof(KeyRec) == 24, "a key record is 24 bytes");

// ---- the other commands ----
enum Kind : uint8_t {
    UP_STAGING,        // vkCmdCopyBufferToImage from a staging buffer (upload)
    UP_UNSWIZZLE_GPU,  // a compute dispatch that reads the texture's storage in its imported pool (upload_unswizzle_gpu)
    UP_ASTC_GPU,       // ASTC decoded by compute dispatches (upload_astc_gpu)
    UP_ZERO_COLOR,     // the zero upload: vkCmdClearColorImage in place of the copy (upload_zeros)
    UP_ZERO_DS,        // the zero upload of a depth image: vkCmdClearDepthStencilImage
    BC_ENCODE,         // a decoded ASTC texture re-encoded as BC3 (a dispatch and a copy)
    CLEAR_NEW_COLOR,   // a new image's clear: vkCmdClearColorImage (create)
    CLEAR_NEW_DS,      // a new depth image's clear: vkCmdClearDepthStencilImage
    CLEAR_TEX_COLOR,   // the game's clear of a whole texture level: vkCmdClearColorImage (clear_tex)
    CLEAR_TEX_DS,      // the same of a depth texture: vkCmdClearDepthStencilImage
    CLEAR_TEX_ATT,     // the game's clear of a region or of a 3D texture: a pass and vkCmdClearAttachments
    CLEAR_ATT_COLOR,   // a bound colour target's clear: vkCmdClearAttachments (clear_color_target)
    CLEAR_ATT_DS,      // the bound depth-stencil target's clear: vkCmdClearAttachments (clear_depth_stencil)
    CLEAR_MASKED,      // a colour target's clear with a channel mask: a full-screen triangle
    CLEAR_SWAP,        // the swapchain image's clear (bars, or no picture): vkCmdClearColorImage (present_image)
    COPY_B2B,          // the game's buffer copy: vkCmdCopyBuffer
    FILL_BUF,          // the game's buffer clear: vkCmdFillBuffer
    COPY_B2T,          // the game's buffer -> texture copy
    COPY_T2B,          // the game's texture -> buffer copy
    COPY_T2T,          // the game's texture copy: vkCmdCopyImage
    BLIT_T2T,          // the same scaled or converted: vkCmdBlitImage
    RESOLVE_T2T,       // the same from a multisampled texture: vkCmdResolveImage
    BLIT_PRESENT,      // the present's blit into the swapchain image
    BLIT_STUB,         // the upscale stub's blit
    MIRROR_SYNC,       // a pool's pages copied to its mirror
    WRITE_BACK,        // a mirror's pages copied back to its pool
    READBACK,          // a texture read back for the CPU: vkCmdCopyImageToBuffer in a submit of its own
    HOST_DISPATCH,     // a dispatch of one of the renderer's own compute shaders (the upscale stand-ins)
    SPARSE_BIND,       // vkQueueBindSparse
    IMPORT,            // a range of guest memory imported
    IMPORT_BACK,       // an import destroyed
    IMPORT_REFUSED,    // an import the driver or the window refused
    IMG_DESTROY,       // images destroyed
    IMG_POOLED,        // render target images kept for the next of their shape
    KINDS
};
enum : uint8_t { S_EXTENT, S_BYTES, S_VALUE, S_NONE };   // what a detail's number is: the largest extent, bytes summed, the newest value, nothing
struct KindInfo {
    const char* name;
    uint8_t size;
    bool by_format;   // (its first uses are told apart by format too: the clears)
};
inline const KindInfo& kind_info(unsigned k) {
    static const KindInfo info[KINDS + 1] = {
        {"upload:staging-copy", S_EXTENT, false},
        {"upload:gpu-unswizzle", S_EXTENT, false},
        {"upload:gpu-astc", S_EXTENT, false},
        {"upload:zero/ClearColorImage", S_EXTENT, true},
        {"upload:zero/ClearDepthStencilImage", S_EXTENT, true},
        {"bc-encode", S_EXTENT, false},
        {"clear:new-image/ClearColorImage", S_EXTENT, true},
        {"clear:new-image/ClearDepthStencilImage", S_EXTENT, true},
        {"clear:texture/ClearColorImage", S_EXTENT, true},
        {"clear:texture/ClearDepthStencilImage", S_EXTENT, true},
        {"clear:texture/ClearAttachments", S_EXTENT, true},
        {"clear:target/ClearAttachments", S_EXTENT, true},
        {"clear:depth-target/ClearAttachments", S_EXTENT, true},
        {"clear:target/masked-draw", S_EXTENT, true},
        {"clear:swapchain/ClearColorImage", S_EXTENT, true},
        {"copy:buffer-buffer", S_BYTES, false},
        {"fill:buffer", S_BYTES, false},
        {"copy:buffer-texture", S_EXTENT, false},
        {"copy:texture-buffer", S_EXTENT, false},
        {"copy:texture-texture", S_EXTENT, false},
        {"blit:texture-texture", S_EXTENT, false},
        {"resolve:texture-texture", S_EXTENT, false},
        {"blit:present", S_EXTENT, false},
        {"blit:upscale-stub", S_EXTENT, false},
        {"copy:mirror-sync", S_BYTES, false},
        {"copy:write-back", S_BYTES, false},
        {"copy:readback", S_EXTENT, false},
        {"dispatch:host-shader", S_EXTENT, false},
        {"sparse-bind", S_NONE, false},
        {"import:made", S_BYTES, false},
        {"import:given-back", S_BYTES, false},
        {"import:refused", S_BYTES, false},
        {"image:destroyed", S_VALUE, false},
        {"image:to-target-pool", S_VALUE, false},
        {"?", S_NONE, false},
    };
    return info[k < (unsigned)KINDS ? k : (unsigned)KINDS];
}
enum : uint8_t { D_DIM = 3, D_FIRST = 4, D_FOLDED = 8 };   // a detail's flags: 0 no dimension, 1 1D, 2 2D, 3 3D; first of the run; formats folded
struct Detail {
    uint8_t kind, flags;
    uint16_t count;     // (saturating)
    uint32_t format;    // the image's VkFormat (0: none)
    uint16_t w, h, d;   // S_EXTENT: the largest; S_BYTES, S_VALUE: a 48-bit number (num)
    uint16_t nvn;       // the texture's NVN format (0: not known)
    uint64_t num() const { return (uint64_t)w | (uint64_t)h << 16 | (uint64_t)d << 32; }
    void set_num(uint64_t v) {
        if (v >> 48) v = (1ull << 48) - 1;
        w = (uint16_t)v, h = (uint16_t)(v >> 16), d = (uint16_t)(v >> 32);
    }
};
static_assert(sizeof(Detail) == 16, "a detail is 16 bytes");

/// What one open recording holds: written by the one thread that records it
struct Acc {
    int32_t last;   // the key used last (-1: none)
    uint16_t nkeys, ndet;
    uint32_t draws, dispatches, fb_dispatches, key_lost, det_lost;
    uint8_t hash[kAccHash];   // a key's place in keys, plus 1
    KeyRec keys[kAccKeys];
    Detail det[kAccDet];
    void reset() {
        last = -1, nkeys = ndet = 0;
        draws = dispatches = fb_dispatches = key_lost = det_lost = 0;
        std::memset(hash, 0, sizeof hash);
    }
    bool empty() const { return !nkeys && !ndet && !draws && !dispatches && !fb_dispatches && !key_lost && !det_lost; }
};
static_assert(kAccKeys < 256, "a key's place fits a byte of the hash");

// ---- the entries ----
enum EntryKind : uint8_t { E_REC = 1, E_FLUSH, E_SEGMENT, E_PRESENT, E_READBACK, E_SPARSE, E_OTHER };
enum : uint8_t { CALL_NONE, CALL_IN, CALL_BACK };
struct Entry {
    uint64_t ordinal;   // its number in the run, from 1 (0: never written)
    uint64_t seq;       // E_REC: its number on the recordings' timeline (0: none). E_FLUSH: the newest one its batch signals
    uint64_t value;     // E_REC: the submission it was recorded for
    uint64_t signal;    // the value its submit signals on the submissions' timeline (0: none)
    uint64_t batch;     // E_REC: the batch it is held for (0: submitted by itself). E_FLUSH: the batch sent
    uint64_t t_us;      // when it was written
    uint64_t key_first, det_first;   // where its keys and details start in their rings (running numbers)
    uint32_t call_us;   // how long the driver's call took (CALL_BACK)
    int32_t tid, result;
    uint32_t draws, dispatches, fb_dispatches, key_lost;
    uint16_t key_n, det_n, det_lost, ncb;
    uint8_t kind, call, slot, reason;
};

enum State : uint8_t { ST_DONE, ST_ORDER, ST_PENDING, ST_CALL, ST_HELD, ST_UNKNOWN };
inline const char* state_name(uint8_t s) {
    static const char* const names[] = {"done", "done (a later one is)", "NOT DONE", "CALL NOT BACK", "held (not sent yet)", "not known"};
    return names[s < 6 ? s : 5];
}

/// The renderer's timelines when a dump is taken (~0: not known)
struct Now {
    uint64_t value_done = ~0ull, rec_done = ~0ull, batch_done = ~0ull;
    uint64_t handed = 0;   // submission values handed out
    uint64_t us = 0;
};

struct What {
    uint64_t count, first_entry, last_entry, first_us, last_us;
    uint32_t format;
    uint16_t nvn;
    uint8_t kind, dim;
};
struct NewProg {
    uint64_t key, entry, us;
    uint8_t flags;
};

/// A dump's lines as they are made: tokens, wrapped at kLine with what a continued line starts with
struct Text {
    char* buf = nullptr;
    size_t cap = 0, len = 0;
    int lines = 0, n = 0;
    bool cut = false;
    char line[kLine + 8];
    char cont[96] = "";
    void reset(char* b, size_t c) { buf = b, cap = c, len = 0, lines = 0, n = 0, cut = false, cont[0] = 0; }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void tok(const char* fmt, ...) {
        char t[320];
        std::va_list a;
        va_start(a, fmt);
        int m = std::vsnprintf(t, sizeof t, fmt, a);
        va_end(a);
        if (m < 0) return;
        if (m >= (int)sizeof t) m = (int)sizeof t - 1;
        if (n && n + m > kLine) {
            end();
            n = std::snprintf(line, sizeof line, "%s", cont);
        }
        if (n + m > kLine) m = kLine - n;
        std::memcpy(line + n, t, (size_t)m), n += m;
    }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void set_cont(const char* fmt, ...) {
        std::va_list a;
        va_start(a, fmt);
        std::vsnprintf(cont, sizeof cont, fmt, a);
        va_end(a);
    }
    void end(bool closing = false) {   // (closing: the dump's last line, which a full buffer still has room for)
        while (n && line[n - 1] == ' ') n--;
        if (!n) return;
        if (len + (size_t)n + 1 + (closing ? 0 : (size_t)kLine + 1) > cap) {
            cut = true, n = 0;
            return;
        }
        std::memcpy(buf + len, line, (size_t)n);
        buf[len + (size_t)n] = '\n';
        len += (size_t)n + 1, lines++, n = 0;
    }
};

/// The lines of a dump through sink, each "gpu trail: d<id> <k>/<lines>: ..." (a line says which dump it is of and where in it)
template <class Sink>
inline void emit(const Text& t, unsigned id, Sink&& sink) {
    int k = 0;
    for (size_t at = 0; at < t.len;) {
        const char* e = (const char*)std::memchr(t.buf + at, '\n', t.len - at);
        const size_t n = e ? (size_t)(e - (t.buf + at)) : t.len - at;
        char out[kLine + 64];
        std::snprintf(out, sizeof out, "gpu trail: d%u %d/%d: %.*s", id, ++k, t.lines, (int)n, t.buf + at);
        sink(out);
        at += n + 1;
    }
}

struct Trail {
    // the rings (lock)
    Entry entries[kEntries];
    KeyRec keys[kKeyRing];
    Detail dets[kDetRing];
    uint64_t n_entries = 0, n_keys = 0, n_dets = 0;   // written so far
    // what is open
    Acc acc[kSlots];   // each: its recording's thread
    Acc loose;         // notes made outside any recording, for the next entry (lock)
    std::atomic<const void*> cmd_key[kCmds];   // a recording's Acc by its command buffer: only added to (bind_cmd), read with no lock
    std::atomic<Acc*> cmd_acc[kCmds];
    std::atomic<uint64_t> unplaced{0};         // draws and dispatches into a command buffer no recording was bound to
    // firsts
    std::atomic<uint64_t> seen[kSeen];
    std::atomic<uint32_t> seen_n{0};
    What whats[kWhats];   // (lock)
    int n_whats = 0;
    uint64_t whats_lost = 0;
    NewProg new_progs[kNewProgs];   // (lock)
    uint64_t n_new_progs = 0;
    // where the queue is (lock)
    uint64_t rec_last = 0, batch_sent = 0, call_batch = 0;
    std::atomic<uint64_t> in_call{0};   // the entry whose driver call is not back (0: none)
    std::atomic<const char*> other_call{nullptr};   // a call on the queue that is no entry (the present itself), while it lasts
    std::atomic<uint64_t> other_us{0};
    // dumps
    uint64_t dumps = 0, dump_entries = 0, dump_us = 0;
    Now dump_now;
    uint8_t st[kEntries];   // (a dump's scratch: one dump at a time)

    uint64_t (*now_us)() = nullptr;   // the clock: read once an entry (push, back), never for a draw or a note
    uint64_t recent_us = 0;           // (lock) the newest entry's time: what a note's first and last use are stamped with
    std::atomic<bool> busy{false};

    void lock() {
        for (int spins = 0; busy.exchange(true, std::memory_order_acquire);)
            while (busy.load(std::memory_order_relaxed))
                if (++spins > 256) std::this_thread::yield();
    }
    void unlock() { busy.store(false, std::memory_order_release); }
    uint64_t clock() const { return now_us ? now_us() : 0; }

    // ---- a recording's place, by its command buffer ----
    static uint32_t cmd_hash(const void* cmd) { return (uint32_t)(((uint64_t)(uintptr_t)cmd * 0x9E3779B97F4A7C15ull) >> 56) & (kCmds - 1); }
    /// The Acc bound to cmd (null: none). Any thread, no lock: a multiply, a load and a compare when it is found at once
    Acc* by_cmd(const void* cmd) const {
        for (uint32_t h = cmd_hash(cmd), probes = 0; probes < (uint32_t)kCmds; probes++, h = (h + 1) & (kCmds - 1)) {
            const void* k = cmd_key[h].load(std::memory_order_acquire);
            if (k == cmd) return cmd_acc[h].load(std::memory_order_relaxed);
            if (!k) return nullptr;
        }
        return nullptr;
    }
    /// cmd is recorded into a from here on (a recording begins). False: the table is full (its draws are then counted as unplaced)
    bool bind_cmd(const void* cmd, Acc* a) {
        if (!cmd) return false;
        lock();
        bool ok = false;
        for (uint32_t h = cmd_hash(cmd), probes = 0; probes < (uint32_t)kCmds / 2 && !ok; probes++, h = (h + 1) & (kCmds - 1)) {
            const void* k = cmd_key[h].load(std::memory_order_relaxed);
            if (k && k != cmd) continue;
            cmd_acc[h].store(a, std::memory_order_relaxed);   // (before its key: who finds the key finds the Acc)
            if (!k) cmd_key[h].store(cmd, std::memory_order_release);
            ok = true;
        }
        unlock();
        return ok;
    }
    void draw_cmd(const void* cmd, uint64_t key, uint8_t flags) {
        if (Acc* a = by_cmd(cmd)) draw(*a, key, flags);
        else unplaced.fetch_add(1, std::memory_order_relaxed);
    }
    void dispatch_cmd(const void* cmd, uint64_t key, uint8_t flags, uint32_t x, uint32_t y, uint32_t z) {
        if (Acc* a = by_cmd(cmd)) dispatch(*a, key, flags, x, y, z);
        else unplaced.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- programs ----
    /// Whether key was never used before in the run (and it is noted as used). Any thread
    bool first_use(uint64_t key) {
        if (!key) return false;
        uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 50) & (kSeen - 1);
        for (int probes = 0; probes < 64; probes++, h = (h + 1) & (kSeen - 1)) {
            uint64_t have = seen[h].load(std::memory_order_relaxed);
            if (have == key) return false;
            if (have) continue;
            if (seen_n.load(std::memory_order_relaxed) >= kSeen / 4 * 3) return false;   // (full: first uses are no longer told)
            if (seen[h].compare_exchange_strong(have, key, std::memory_order_relaxed)) {
                seen_n.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (have == key) return false;
        }
        return false;
    }
    bool seen_full() const { return seen_n.load(std::memory_order_relaxed) >= kSeen / 4 * 3; }

#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    KeyRec* find(Acc& a, uint64_t key, uint8_t flags) {
        uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 56);
        for (int probes = 0; probes < kAccHash; probes++, h = (h + 1) & (kAccHash - 1)) {
            const unsigned i = a.hash[h];
            if (i) {
                if (i <= kAccKeys && a.keys[i - 1].key == key) {
                    a.last = (int32_t)i - 1;
                    return &a.keys[i - 1];
                }
                continue;
            }
            if (a.nkeys >= kAccKeys) break;
            const unsigned at = a.nkeys++;
            a.hash[h] = (uint8_t)(at + 1);
            KeyRec& k = a.keys[at];
            k = KeyRec{};
            k.key = key;
            if (first_use(key)) {
                k.flags = K_FIRST;
                lock();
                new_progs[n_new_progs++ % kNewProgs] = {key, n_entries + 1, recent_us, flags};
                unlock();
            }
            a.last = (int32_t)at;
            return &k;
        }
        a.key_lost++;
        return nullptr;
    }
    /// A draw of program key recorded into a (flags: K_DRAW, K_MESH or K_FALLBACK). The key of the use before: two increments
    void draw(Acc& a, uint64_t key, uint8_t flags) {
        a.draws++;
        KeyRec* k = a.last >= 0 && a.keys[a.last].key == key ? &a.keys[a.last] : find(a, key, flags);
        if (!k) return;
        if (k->draws != 0xFFFF) k->draws++;
        k->flags |= flags;
    }
    /// A dispatch of program key recorded into a (flags: K_DISPATCH or K_FB_DISPATCH, with K_INDIRECT), x * y * z workgroups
    void dispatch(Acc& a, uint64_t key, uint8_t flags, uint32_t x, uint32_t y, uint32_t z) {
        if (flags & K_FB_DISPATCH) a.fb_dispatches++;
        else a.dispatches++;
        KeyRec* k = a.last >= 0 && a.keys[a.last].key == key ? &a.keys[a.last] : find(a, key, flags);
        if (!k) return;
        if (k->disp != 0xFFFF) k->disp++;
        k->flags |= flags;
        const uint64_t g = (uint64_t)x * y * z, sum = (uint64_t)k->groups + g;
        k->groups = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sum;
        if (g >= (uint64_t)k->x * k->y * k->z)
            k->x = (uint16_t)(x > 0xFFFF ? 0xFFFF : x), k->y = (uint16_t)(y > 0xFFFF ? 0xFFFF : y), k->z = (uint16_t)(z > 0xFFFF ? 0xFFFF : z);
    }

    // ---- the other commands ----
    /// One command of kind into a (null: outside any recording, for the next entry). dim: 0 none, 1 to 3; format, nvn: the image's
    /// formats (0: none); w, h, d: its extent (S_EXTENT); num: bytes (S_BYTES) or a value (S_VALUE). Any thread: the lock
    void note(Acc* a, Kind kind, unsigned dim, uint32_t format, uint32_t nvn, uint32_t w, uint32_t h, uint32_t d, uint64_t num) {
        const KindInfo& info = kind_info(kind);
        const uint32_t what_format = info.by_format ? format : 0;
        dim &= D_DIM;
        lock();
        bool first = false;
        {   // the run's table
            What* row = nullptr;
            for (int i = n_whats - 1; i >= 0 && !row; i--)
                if (whats[i].kind == kind && whats[i].dim == dim && whats[i].format == what_format) row = &whats[i];
            if (!row && n_whats < kWhats) {
                row = &whats[n_whats++];
                *row = What{};
                row->kind = kind, row->dim = (uint8_t)dim, row->format = what_format, row->nvn = (uint16_t)(info.by_format ? nvn : 0);
                row->first_entry = n_entries + 1, row->first_us = recent_us;
                first = true;
            }
            if (row) row->count++, row->last_entry = n_entries + 1, row->last_us = recent_us;
            else whats_lost++;
        }
        Acc& to = a ? *a : loose;
        Detail* at = nullptr;
        for (int i = 0; i < to.ndet && !at; i++)
            if (to.det[i].kind == kind && (to.det[i].flags & D_DIM) == dim && to.det[i].format == format && !(to.det[i].flags & D_FOLDED)) at = &to.det[i];
        if (!at && to.ndet < kAccDet) {
            at = &to.det[to.ndet++];
            *at = Detail{};
            at->kind = kind, at->flags = (uint8_t)dim, at->format = format, at->nvn = (uint16_t)nvn;
        }
        if (!at) {   // (the recording's table is full: with any of its kind, the formats folded)
            for (int i = 0; i < to.ndet && !at; i++)
                if (to.det[i].kind == kind) at = &to.det[i];
            if (at) at->flags |= D_FOLDED;
        }
        if (!at) {
            to.det_lost++;
        } else {
            if (at->count != 0xFFFF) at->count++;
            if (first) at->flags |= D_FIRST;
            if (info.size == S_EXTENT) {
                const auto c16 = [](uint32_t v) { return (uint16_t)(v > 0xFFFF ? 0xFFFF : v); };
                if ((uint64_t)w * h * (d ? d : 1) >= (uint64_t)at->w * at->h * (at->d ? at->d : 1)) at->w = c16(w), at->h = c16(h), at->d = c16(d);
            } else if (info.size == S_BYTES) {
                at->set_num(at->num() + (num >> 48 ? (1ull << 48) - 1 : num));
            } else if (info.size == S_VALUE) {
                if (num > at->num()) at->set_num(num);
            }
        }
        unlock();
    }

    // ---- entries ----
    void take(Entry& e, Acc& a) {   // (lock) a's keys and details after e's in the rings, a emptied
        for (int i = 0; i < a.nkeys; i++) keys[n_keys++ % kKeyRing] = a.keys[i];
        for (int i = 0; i < a.ndet; i++) dets[n_dets++ % kDetRing] = a.det[i];
        e.key_n = (uint16_t)(e.key_n + a.nkeys), e.det_n = (uint16_t)(e.det_n + a.ndet);
        e.draws += a.draws, e.dispatches += a.dispatches, e.fb_dispatches += a.fb_dispatches, e.key_lost += a.key_lost;
        e.det_lost = (uint16_t)(e.det_lost + a.det_lost);
        a.reset();
    }
    /// An entry written: what a (null: no recording) holds and the notes made outside any recording go with it. e.call CALL_IN:
    /// the driver is called for it next (back(): it returned). Returns its number in the run
    uint64_t push(Entry e, Acc* a) {
        e.t_us = clock();
        lock();
        e.ordinal = ++n_entries;
        if (e.t_us > recent_us) recent_us = e.t_us;
        e.key_first = n_keys, e.det_first = n_dets;
        e.key_n = e.det_n = e.det_lost = 0;
        e.draws = e.dispatches = e.fb_dispatches = e.key_lost = 0;
        if (a) take(e, *a);
        if (!loose.empty()) take(e, loose);
        if (e.kind == E_REC && e.seq > rec_last) rec_last = e.seq;
        if (e.kind == E_FLUSH && e.batch > batch_sent) batch_sent = e.batch;
        if (e.call == CALL_IN) {
            in_call.store(e.ordinal, std::memory_order_relaxed);
            call_batch = e.kind == E_FLUSH ? e.batch : 0;
        }
        entries[(e.ordinal - 1) % kEntries] = e;
        unlock();
        return e.ordinal;
    }
    /// The driver's call made for entry ordinal came back with result
    void back(uint64_t ordinal, int32_t result) {
        if (!ordinal) return;
        const uint64_t t = clock();
        lock();
        Entry& e = entries[(ordinal - 1) % kEntries];
        if (e.ordinal == ordinal) {
            const uint64_t took = t > e.t_us ? t - e.t_us : 0;
            e.call = CALL_BACK, e.result = result, e.call_us = took > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)took;
        }
        if (in_call.load(std::memory_order_relaxed) == ordinal) in_call.store(0, std::memory_order_relaxed), call_batch = 0;
        unlock();
    }

    /// A call on the queue that submits nothing (vkQueuePresentKHR) begins, and ends: one at a time (the presents take turns)
    void other_begin(const char* what) {
        other_us.store(clock(), std::memory_order_relaxed);
        other_call.store(what, std::memory_order_release);
    }
    void other_end() { other_call.store(nullptr, std::memory_order_release); }

    // ---- the dump ----
    uint64_t oldest() const { return n_entries > kEntries ? n_entries - kEntries + 1 : 1; }   // (lock) the oldest entry kept
    uint8_t own_state(const Entry& e, const Now& now) const {   // (lock) what the timelines say of e by itself
        if (e.call == CALL_IN) return ST_CALL;
        if (e.kind == E_REC && e.batch) {
            if (now.batch_done != ~0ull && e.batch <= now.batch_done) return ST_DONE;
            if (now.batch_done == ~0ull && e.seq && now.rec_done != ~0ull && e.seq <= now.rec_done) return ST_DONE;
            if (e.batch > batch_sent) return ST_HELD;
            if (call_batch && e.batch == call_batch) return ST_CALL;
            return now.batch_done != ~0ull || (e.seq && now.rec_done != ~0ull) ? ST_PENDING : ST_UNKNOWN;
        }
        if (e.kind == E_REC) {
            if (e.seq && now.rec_done != ~0ull) return e.seq <= now.rec_done ? ST_DONE : ST_PENDING;
            if (e.signal && now.value_done != ~0ull) return e.signal <= now.value_done ? ST_DONE : ST_PENDING;
            return ST_UNKNOWN;
        }
        if (e.kind == E_FLUSH && now.batch_done != ~0ull) return e.batch <= now.batch_done ? ST_DONE : ST_PENDING;
        return ST_UNKNOWN;
    }
    static void seconds(char* out, size_t cap, uint64_t us) { std::snprintf(out, cap, "%.3f", (double)us / 1e6); }
    static void value_text(char* out, size_t cap, uint64_t v) {
        if (v == ~0ull) std::snprintf(out, cap, "?");
        else std::snprintf(out, cap, "%llu", (unsigned long long)v);
    }
    static const char* dim_name(unsigned dim) {
        static const char* const names[] = {"", " 1D", " 2D", " 3D"};
        return names[dim & D_DIM];
    }
    static const char* entry_name(uint8_t kind) {
        static const char* const names[] = {"?", "rec", "flush", "segment", "present", "readback", "sparse-bind", "other"};
        return names[kind <= E_OTHER ? kind : 0];
    }
    static void key_tok(const KeyRec& k, Text& t) {
        char s[112];
        int n = std::snprintf(s, sizeof s, " %016llx%s:", (unsigned long long)k.key, k.flags & K_FIRST ? "*" : "");
        if (k.draws) n += std::snprintf(s + n, sizeof s - (size_t)n, "%u%s", (unsigned)k.draws, k.flags & K_MESH ? "M" : k.flags & K_FALLBACK ? "F" : "");
        if (k.disp)
            std::snprintf(s + n, sizeof s - (size_t)n, "%s%c%u%s/%uwg(%ux%ux%u)", k.draws ? "+" : "", k.flags & K_FB_DISPATCH ? 'f' : 'c', (unsigned)k.disp,
                          k.flags & K_INDIRECT ? "i" : "", k.groups, (unsigned)k.x, (unsigned)k.y, (unsigned)k.z);
        t.tok("%s", s);
    }
    static void det_tok(const Detail& d, bool more, Text& t) {
        const KindInfo& info = kind_info(d.kind);
        char s[160];
        int n = std::snprintf(s, sizeof s, " %s%s", info.name, dim_name(d.flags));
        if (d.format || d.nvn) {
            n += std::snprintf(s + n, sizeof s - (size_t)n, " fmt %u", d.format);
            if (d.nvn) n += std::snprintf(s + n, sizeof s - (size_t)n, "/%#x", (unsigned)d.nvn);
        }
        if (info.size == S_EXTENT) n += std::snprintf(s + n, sizeof s - (size_t)n, " %ux%ux%u", (unsigned)d.w, (unsigned)d.h, (unsigned)d.d);
        else if (info.size == S_BYTES) n += std::snprintf(s + n, sizeof s - (size_t)n, " %llu KB", (unsigned long long)((d.num() + 1023) >> 10));
        else if (info.size == S_VALUE) n += std::snprintf(s + n, sizeof s - (size_t)n, " (used up to value %llu)", (unsigned long long)d.num());
        std::snprintf(s + n, sizeof s - (size_t)n, " x%u%s%s%s", (unsigned)d.count, d.flags & D_FIRST ? "*" : "", d.flags & D_FOLDED ? "~" : "", more ? "," : "");
        t.tok("%s", s);
    }
    void entry_text(const Entry& e, uint8_t state, Text& t) const {   // (lock) one entry: a line, and more when its keys ask
        char when[32];
        seconds(when, sizeof when, e.t_us);
        t.set_cont("#%llu %s (more):", (unsigned long long)e.ordinal, entry_name(e.kind));
        t.tok("#%llu %s", (unsigned long long)e.ordinal, entry_name(e.kind));
        if (e.kind == E_REC) {
            t.tok(" %llu %s:", (unsigned long long)e.seq, state_name(state));
            if (e.batch) t.tok(" batch %llu,", (unsigned long long)e.batch);
            else t.tok(" its own vkQueueSubmit2,");
            t.tok(" value %llu,", (unsigned long long)e.value);
            if (e.signal) t.tok(" signals %llu,", (unsigned long long)e.signal);
        } else if (e.kind == E_FLUSH) {
            t.tok(" %s: batch %llu by vkQueueSubmit2,", state_name(state), (unsigned long long)e.batch);
            if (e.signal) t.tok(" signals value %llu,", (unsigned long long)e.signal);
            if (e.seq) t.tok(" recordings to %llu,", (unsigned long long)e.seq);
            static const char* const why[] = {"other", "poll", "wait", "slot", "present", "cap", "value"};
            t.tok(" reason %s,", e.reason < 7 ? why[e.reason] : "?");
        } else {
            t.tok(" %s:", state_name(state));
        }
        t.tok(" %u cb,", (unsigned)e.ncb);
        if (e.kind == E_REC || e.kind == E_SEGMENT) t.tok(" slot %u,", (unsigned)e.slot);
        t.tok(" thread %d, %s s", (int)e.tid, when);
        if (e.call == CALL_IN) t.tok(", THE DRIVER'S CALL IS NOT BACK");
        else if (e.call == CALL_BACK) t.tok(", call %.2f ms -> %d", (double)e.call_us / 1e3, (int)e.result);
        if (e.draws || e.dispatches || e.fb_dispatches)
            t.tok("; %u draws, %u dispatches, %u fallback dispatches", e.draws, e.dispatches, e.fb_dispatches);
        if (e.key_lost) t.tok(" (%u of them with programs past the %d kept)", e.key_lost, kAccKeys);
        if (e.key_n) {
            if (e.key_first + kKeyRing < n_keys) {
                t.tok("; its %u program keys were overwritten", (unsigned)e.key_n);
            } else {
                t.tok("; keys:");
                for (unsigned i = 0; i < e.key_n; i++) key_tok(keys[(e.key_first + i) % kKeyRing], t);
            }
        }
        if (e.det_n) {
            if (e.det_first + kDetRing < n_dets) {
                t.tok("; its %u other commands were overwritten", (unsigned)e.det_n);
            } else {
                t.tok("; other:");
                for (unsigned i = 0; i < e.det_n; i++) det_tok(dets[(e.det_first + i) % kDetRing], i + 1 < e.det_n, t);
            }
        }
        if (e.det_lost) t.tok("; %u more commands not kept", (unsigned)e.det_lost);
        t.end();
    }
    /// Whether a dump taken now would say more than the last one did: an entry was written since, or a timeline moved
    bool moved(const Now& now) {
        lock();
        const bool m = !dumps || n_entries != dump_entries || now.value_done != dump_now.value_done || now.rec_done != dump_now.rec_done ||
                       now.batch_done != dump_now.batch_done || in_call.load(std::memory_order_relaxed) != dump_call;
        unlock();
        return m;
    }
    uint64_t dump_call = 0;
    /// The same asked before the driver is asked for its timelines: whether an entry was written, or the driver's call changed,
    /// since the last dump. *written: the entries so far; *call: the entry whose driver call is not back (0: none)
    bool written_since_dump(uint64_t* written, uint64_t* call) {
        lock();
        *written = n_entries, *call = in_call.load(std::memory_order_relaxed);
        const bool w = !dumps || n_entries != dump_entries || *call != dump_call;
        unlock();
        return w;
    }
    /// The dump's text into t (reset by the caller): the state of the timelines and of the driver's call, what the run did for
    /// the first time, then the entries, oldest first: kBefore completed ones, kFirstBad from the first that is not, and the
    /// newest kTail. Returns the number of lines. One dump at a time (the caller's to see to)
    int dump(const Now& now, Text& t) {
        lock();
        const uint64_t first = oldest(), last = n_entries;
        // each entry's state, newest first: one the timelines say nothing of is done when a later one is (one queue, in order)
        bool later_done = false;
        uint64_t first_bad = 0, bad = 0;
        for (uint64_t o = last; o >= first && o; o--) {
            const Entry& e = entries[(o - 1) % kEntries];
            uint8_t s = own_state(e, now);
            if (s == ST_UNKNOWN && later_done) s = ST_ORDER;
            if (s == ST_DONE) later_done = true;
            st[(o - 1) % kEntries] = s;
            if (s == ST_PENDING || s == ST_CALL || s == ST_HELD) first_bad = o, bad++;
        }
        char a[32], b[32], c[32], when[32];
        value_text(a, sizeof a, now.value_done), value_text(b, sizeof b, now.rec_done), value_text(c, sizeof c, now.batch_done);
        seconds(when, sizeof when, now.us);
        t.set_cont("(state, more):");
        t.tok("state at %s s: submission values done to %s of %llu handed out;", when, a, (unsigned long long)now.handed);
        t.tok(" recordings done to %s of %llu submitted;", b, (unsigned long long)rec_last);
        t.tok(" batches done to %s of %llu sent;", c, (unsigned long long)batch_sent);
        if (const uint64_t o = in_call.load(std::memory_order_relaxed)) {
            const Entry& e = entries[(o - 1) % kEntries];
            if (e.ordinal == o) {
                seconds(when, sizeof when, e.t_us);
                t.tok(" THE DRIVER'S CALL IS NOT BACK: #%llu %s", (unsigned long long)o, entry_name(e.kind));
                if (e.batch) t.tok(" batch %llu", (unsigned long long)e.batch);
                t.tok(" (%u cb) entered at %s s on thread %d;", (unsigned)e.ncb, when, (int)e.tid);
            } else {
                t.tok(" THE DRIVER'S CALL IS NOT BACK: #%llu (no longer kept);", (unsigned long long)o);
            }
        } else {
            t.tok(" no submit is in the driver's call;");
        }
        if (const char* what = other_call.load(std::memory_order_acquire)) {
            seconds(when, sizeof when, other_us.load(std::memory_order_relaxed));
            t.tok(" %s, ENTERED AT %s s, IS NOT BACK;", what, when);
        }
        if (first_bad) {
            const Entry& e = entries[(first_bad - 1) % kEntries];
            seconds(when, sizeof when, e.t_us);
            t.tok(" %llu entries not done, the first #%llu %s", (unsigned long long)bad, (unsigned long long)first_bad, entry_name(e.kind));
            if (e.kind == E_REC) t.tok(" %llu", (unsigned long long)e.seq);
            if (e.batch) t.tok(" (batch %llu)", (unsigned long long)e.batch);
            t.tok(" written at %s s (%s);", when, state_name(st[(first_bad - 1) % kEntries]));
            if (first_bad == first && first > 1) t.tok(" IT IS THE OLDEST ENTRY KEPT: the first that did not finish may be older, and overwritten;");
        } else {
            t.tok(" every entry kept is done or not known;");
        }
        // what is shown: kBefore before the first not done (none: the last kBefore), kFirstBad from it, and the newest kTail
        uint64_t from = first_bad ? (first_bad > first + kBefore ? first_bad - kBefore : first) : (last > first + kBefore ? last - kBefore + 1 : first);
        if (!last) from = 1;
        uint64_t skip_from = 0, skip_to = 0;   // (the middle left out when there are too many)
        if (first_bad && last - first_bad + 1 > (uint64_t)(kFirstBad + kTail)) skip_from = first_bad + kFirstBad, skip_to = last - kTail;
        t.tok(" the ring keeps #%llu to #%llu, shown from #%llu", (unsigned long long)(last ? first : 0), (unsigned long long)last, (unsigned long long)from);
        if (skip_from) t.tok(" (#%llu to #%llu left out)", (unsigned long long)skip_from, (unsigned long long)skip_to);
        if (const uint64_t u = unplaced.load(std::memory_order_relaxed)) t.tok("; %llu draws and dispatches of the run were recorded into no known recording", (unsigned long long)u);
        t.end();
        // the run's firsts
        t.set_cont("(first uses, more):");
        t.tok("first uses in the run, oldest first (kind, image, format Vk/NVN: count, first s #entry, last s #entry):");
        for (int i = 0; i < n_whats; i++) {
            const What& w = whats[i];
            char f[32], l[32], s[200];
            seconds(f, sizeof f, w.first_us), seconds(l, sizeof l, w.last_us);
            int n = std::snprintf(s, sizeof s, " %s%s", kind_info(w.kind).name, dim_name(w.dim));
            if (w.format || w.nvn) {
                n += std::snprintf(s + n, sizeof s - (size_t)n, " fmt %u", w.format);
                if (w.nvn) n += std::snprintf(s + n, sizeof s - (size_t)n, "/%#x", (unsigned)w.nvn);
            }
            std::snprintf(s + n, sizeof s - (size_t)n, ": x%llu, %s #%llu, %s #%llu;", (unsigned long long)w.count, f, (unsigned long long)w.first_entry, l,
                          (unsigned long long)w.last_entry);
            t.tok("%s", s);
        }
        if (!n_whats) t.tok(" none");
        if (whats_lost) t.tok(" (%llu uses of kinds past the %d kept)", (unsigned long long)whats_lost, kWhats);
        t.end();
        t.set_cont("(new programs, more):");
        t.tok("programs used for the first time: %u in the run%s", seen_n.load(std::memory_order_relaxed), seen_full() ? " (the table is full: no more are told)" : "");
        if (n_new_progs) {
            const uint64_t n0 = n_new_progs > (uint64_t)kNewProgs ? n_new_progs - kNewProgs : 0;
            t.tok("; the last %llu (key, use, s, #entry):", (unsigned long long)(n_new_progs - n0));
            for (uint64_t i = n0; i < n_new_progs; i++) {
                const NewProg& p = new_progs[i % kNewProgs];
                char w2[32];
                seconds(w2, sizeof w2, p.us);
                t.tok(" %016llx %s %s #%llu;", (unsigned long long)p.key,
                      p.flags & K_FB_DISPATCH ? "fallback-dispatch" : p.flags & K_DISPATCH ? "dispatch" : p.flags & K_MESH ? "mesh-draw" : p.flags & K_FALLBACK ? "fallback-draw" : "draw",
                      w2, (unsigned long long)p.entry);
            }
        }
        t.end();
        // the entries
        for (uint64_t o = from; o <= last && last; o++) {
            if (skip_from && o == skip_from) {
                t.tok("(#%llu to #%llu are not shown: %llu entries, all after the first not done)", (unsigned long long)skip_from, (unsigned long long)skip_to,
                      (unsigned long long)(skip_to - skip_from + 1));
                t.end();
                o = skip_to;
                continue;
            }
            entry_text(entries[(o - 1) % kEntries], st[(o - 1) % kEntries], t);
        }
        if (!loose.empty()) {   // (noted since the last entry: no submit has taken them yet)
            t.set_cont("(noted since #%llu, more):", (unsigned long long)last);
            t.tok("noted since #%llu, in no entry yet:", (unsigned long long)last);
            for (int i = 0; i < loose.nkeys; i++) key_tok(loose.keys[i], t);
            for (int i = 0; i < loose.ndet; i++) det_tok(loose.det[i], i + 1 < loose.ndet, t);
            if (loose.det_lost || loose.key_lost) t.tok(" (%u more not kept)", loose.det_lost + loose.key_lost);
            t.end();
        }
        t.set_cont("(end, more):");
        t.tok("end of the dump:");
        if (first_bad) t.tok(" the first entry not done is #%llu, %llu are not done;", (unsigned long long)first_bad, (unsigned long long)bad);
        else t.tok(" no entry kept is known not to be done;");
        t.tok(" key: <16 hex>[*]:<draws>[M mesh shaders|F fallback][+][c|f]<dispatches>[i indirect]/<workgroups>wg(<largest>), * first use in the run, ~ formats folded");
        if (t.cut) t.tok(" (THE TEXT WAS CUT: the dump's buffer is full)");
        t.end(true);
        dumps++, dump_entries = n_entries, dump_us = now.us, dump_now = now, dump_call = in_call.load(std::memory_order_relaxed);
        unlock();
        return t.lines;
    }
};

// ---- waits for a submission, watched by whoever calls Waits::advance once a second or so ----
struct WaitSlot {
    std::atomic<uint32_t> state{0};   // 0 free, else the watcher's tick it began at plus 1
    std::atomic<bool> told{false};
    std::atomic<const char*> what{nullptr};
    std::atomic<uint64_t> value{0};
    std::atomic<int32_t> tid{0};
};
struct Waits {
    WaitSlot slot[kWaits];
    std::atomic<uint32_t> tick{0};
    /// A wait begins: its slot, or -1 when all are taken (it is then not watched)
    int begin(const char* what, uint64_t value, int32_t tid) {
        const uint32_t now = tick.load(std::memory_order_relaxed) + 1;
        for (int i = 0; i < kWaits; i++) {
            uint32_t free = 0;
            if (slot[i].state.load(std::memory_order_relaxed) == 0 && slot[i].state.compare_exchange_strong(free, now, std::memory_order_acquire)) {
                slot[i].what.store(what, std::memory_order_relaxed), slot[i].value.store(value, std::memory_order_relaxed);
                slot[i].tid.store(tid, std::memory_order_relaxed), slot[i].told.store(false, std::memory_order_relaxed);
                return i;
            }
        }
        return -1;
    }
    void end(int i) {
        if (i >= 0 && i < kWaits) slot[i].state.store(0, std::memory_order_release);
    }
    /// A tick of the watcher's clock (a wait has no clock of its own: it is timed in the watcher's ticks)
    void advance() { tick.fetch_add(1, std::memory_order_relaxed); }
    /// A wait that has seen `ticks` of them or more and was not told yet, which is now told (-1: none). A wait that began just
    /// before a tick has seen one at once: one told after n ticks has lasted between n - 1 and n of the watcher's periods
    int overdue(uint32_t ticks) {
        const uint32_t now = tick.load(std::memory_order_relaxed);
        for (int i = 0; i < kWaits; i++) {
            const uint32_t s = slot[i].state.load(std::memory_order_acquire);
            if (s && now + 1 - s >= ticks && !slot[i].told.exchange(true, std::memory_order_relaxed)) return i;
        }
        return -1;
    }
};

} // namespace mkwvk::gpu_trail
