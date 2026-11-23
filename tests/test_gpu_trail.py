"""(ps5) The GPU trail: what the renderer gave the GPU, kept in memory and written out when the GPU stops answering.

On the console the GPU sometimes stops executing: a submission never completes, the presenting thread stays in the driver's
submit or in a wait, and nothing says why. The trail (gpu/vk/gpu_trail.h, vk_gputrail.h, vk_gputrail.cpp; RX_VK_GPU_TRAIL)
keeps the last submits with the programs drawn and dispatched in them and their other commands, and what the run did for
the first time, and writes all that to the stage trail when the queue's lock or a wait for a submission has lasted 5 s, or
the host's watchdog finds no frame for 20 s. It is an instrument: these checks hold it to recording and nothing else.

- its rules are compiled and run alone (tests/test_gpu_trail.cpp): a recording's keys folded, the other commands folded by
  kind, dimension and format, the rings' wrap-around, what is done and what is not by the timelines, the dump's format and
  its lines' lengths, the waits as the watcher sees them, several threads at once;
- the switch is read at the first use and nowhere else, on by default on the console only; off, a draw pays one load and a
  branch and nothing is allocated;
- nothing is recorded into a command buffer and no call on the queue is added: the trail's files name no vkCmd and no
  vkQueue call, and a dump reads the three timelines' values and takes none of the renderer's locks;
- every submit and every other call on the queue is written into the ring before the driver is called and marked when the
  call returns; every clear, upload path, copy, blit, dispatch, draw, sparse bind, import and destroyed image has its
  record call beside it;
- the waits for a submission are watched, with the timeouts they had;
- the dump goes out by the path of the queue's lock line (hos_ps5_trail on the console) from the three places that ask for
  it, its first line before the driver is asked anything, once for a stall, and its lines fit the stage trail's
  (RX_VK_GPU_TRAIL_AT=<seconds>: once more at that time of the run, stall or not, for a run to compare a stall's with);
- the thread that writes a dump is not asked where it is meanwhile: the host's watchdog asks with SIGUSR2, its handler
  answers with a stage-trail line, and the stage trail's lock is not one a thread can take twice;
- where the Vulkan headers are (--headers, HOOKSHOT_VULKAN_HEADERS, or the CPM cache of RX_CPM) and clang++ is, the glue itself
  (gpu/vk/vk_gputrail.cpp) is built with tests/gpu_trail_glue_test.cpp in place of the renderer and run: off by default off
  the console, nothing made and nothing said; on, the dump as the log would get it, once for a stall, and a watched wait.

python -I -B tests/test_gpu_trail.py [--cxx g++] [--glue-cxx clang++] [--headers <Vulkan-Headers/include>] [--bench]
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
RULES = VK / "gpu_trail.h"
CALLS = VK / "vk_gputrail.h"
GLUE = VK / "vk_gputrail.cpp"
INTERNAL = VK / "vk_internal.h"
REPLAY = VK / "vk_replay.cpp"
DRAW = VK / "vk_draw.cpp"
TEXTURE = VK / "vk_texture.cpp"
MEMORY = VK / "vk_memory.cpp"
XFER = VK / "vk_xfer.cpp"
HOST = ROOT / "host" / "hos_thread.c"
EARLY = ROOT / "host" / "posix" / "ps5_early.c"
POSIX = ROOT / "host" / "posix" / "win32_posix.c"
TEST = ROOT / "tests" / "test_gpu_trail.cpp"

NOTE = re.compile(r"\b(gt_note|gt_note_num|gt_note_num_next|gt_note_tex|gt_note_tex_next|trail_note)\(gt::(\w+)")


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def code_of(text):
    """The text without its // comments (a comment may name what the code must not call)"""
    return "\n".join(line.split("//")[0] for line in text.split("\n"))


def before(text, call, wanted, lines=1):
    """Every line of text that has call has wanted in the `lines` lines before it; returns how many there are"""
    rows = text.split("\n")
    n = 0
    for i, row in enumerate(rows):
        if call in row.split("//")[0]:
            n += 1
            near = "\n".join(rows[max(0, i - lines):i])
            need(re.search(wanted, near), "%s without its record call in the %d line(s) before it: %s" % (call, lines, row.strip()))
    return n


def check_rules(cxx, bench):
    with tempfile.TemporaryDirectory(prefix="gpu-trail-") as tmp:
        exe = Path(tmp) / "gpu-trail"
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", str(TEST), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        out = r.stdout + r.stderr
        if r.returncode or "PASSED: 0 failed" not in out or "FAIL " in out:
            print(out[-6000:])
            raise SystemExit("FAIL: the trail's rules (exit %d)" % r.returncode)
        for line in out.split("\n"):
            if line.startswith("     ("):
                print(line.strip(), flush=True)
        example = subprocess.run([str(exe), "--example"], capture_output=True, text=True, timeout=60, check=True).stdout.split("\n")
        need(len(example) > 20 and all(l.startswith("gpu trail: d1 %d/%d: " % (i + 1, len(example) - 1)) for i, l in enumerate(example[:-1])),
             "the example dump's lines are not numbered")
        if bench:
            fast = Path(tmp) / "gpu-trail-o2"
            subprocess.run([cxx, "-std=c++20", "-O2", "-pthread", str(TEST), "-o", str(fast)], check=True)
            print(subprocess.run([str(fast), "--bench"], capture_output=True, text=True, timeout=300, check=True).stdout.rstrip(), flush=True)
    print("PASS: gpu/vk/gpu_trail.h: %d checks of its rules (keys, details, wrap-around, done and not done, the dump's lines, waits, threads)"
          % out.count("\nPASS "), flush=True)


def check_switch(rules, calls, glue):
    every = "".join(p.read_text(encoding="utf-8") for p in sorted(VK.glob("*.cpp")) + sorted(VK.glob("*.h")))
    ask = block(glue, "bool gt_ask()")
    need(every.count('getenv("RX_VK_GPU_TRAIL")') == 1 and 'getenv("RX_VK_GPU_TRAIL")' in ask,
         "RX_VK_GPU_TRAIL is not read inside gt_ask alone (a static initializer would not see the console's settings file, read in main)")
    need(every.count('getenv("RX_VK_GPU_TRAIL_AT")') == 1 and 'getenv("RX_VK_GPU_TRAIL_AT")' in ask and "uint64_t dump_at_us = 0;" in glue and
         "if (dump_at_us && trail_now_us() >= dump_at_us && !at_done.exchange(true))" in block(glue, "void gt_poll()"),
         "RX_VK_GPU_TRAIL_AT is not read at the first use alone, or its dump is not written once, and only when it is set")
    need(re.search(r"#ifdef __PROSPERO__\s+on = !\(e && e\[0\] == '0'\);\s+#else\s+on = e && e\[0\] == '1';\s+#endif", ask),
         "the trail is not on by default on the console and off elsewhere")
    need("std::atomic<int> gt_state{0};" in glue and "gt::Trail* gt_trail = nullptr;" in glue, "the switch does not start as not asked, with no trail made")
    need(every.count("new (std::nothrow) gt::Trail()") == 1 and "new (std::nothrow) gt::Trail()" in ask and "std::malloc(kDumpText)" in ask,
         "the trail's memory is made anywhere but where the switch is found on")
    need(glue.count('"gpu trail: on: ') == 1 and '"gpu trail: on: ' in ask and
         ask.index("gt_state.store(on ? 1 : -1, std::memory_order_release);") < ask.index("    if (on) {   // (the one line at the start") < ask.index('"gpu trail: on: '),
         "the one line that says the trail is on is not gt_ask's, written once the switch is set and its lock let go")
    need("std::lock_guard lk(mu);" in ask and "if (const int s = gt_state.load(std::memory_order_acquire)) return s > 0;" in ask,
         "two threads asking at once could both make the trail")
    # off: a draw and a dispatch pay a load of gt_state and a branch, before the thread-local is touched
    # (and on: its recording by the command buffer, no thread-local: on the console each one is a call, __emutls_get_address)
    for fn in ("inline void gt_draw(const void* cmd, uint64_t key, uint8_t flags)",
               "inline void gt_dispatch(const void* cmd, uint64_t key, uint8_t flags, uint32_t x, uint32_t y, uint32_t z)"):
        body = block(calls, fn)
        first = body[body.index("{") + 1:].strip().split("\n")[0].strip()
        need(first == "if (gt_state.load(std::memory_order_acquire) <= 0) return;" and "gt_acc" not in body and "thread_local" not in body and
             "_cmd(cmd, key, flags" in body, fn + " does not leave on the switch before anything else, or goes by a thread-local")
    need("thread_local" not in block(rules, "struct Trail") and "lock()" not in block(rules, "Acc* by_cmd(const void* cmd) const"),
         "a draw's look-up of its recording takes a lock or a thread-local")
    on = block(calls, "inline bool gt_on()")
    need("return s > 0 || (s == 0 && gt_ask());" in on, "gt_on asks the switch more than once, or not at all")
    for fn in ("inline void gt_note(", "inline void gt_note_num(", "inline void gt_note_num_next(", "inline void gt_note_tex(", "inline void gt_note_tex_next("):
        need("if (gt_on()) gt_trail->note(" in block(calls, fn), fn + " notes without asking the switch")
    need("if (!gt_on()) return 0;" in block(glue, "uint64_t gt_submit(") and "if (!gt_on()) return;" in block(glue, "void gt_begin_recording(int slot, const void* cmd)"),
         "a submit or a recording's start goes on with the trail off")
    need("if (gt_state.load(std::memory_order_acquire) > 0) slot = gt_wait_begin(what, value);" in block(calls, "struct GtWait"),
         "a wait is watched with the trail off")
    need("if (gt_state.load(std::memory_order_acquire) <= 0) return;" in block(glue, "void gpu_trail_dump(const char* why)") and
         "if (gt_state.load(std::memory_order_acquire) <= 0) return;" in block(glue, "void gt_poll()"), "a dump or a tick goes on with the trail off")
    need("inline constinit thread_local gt::Acc* gt_acc = nullptr;" in calls, "the recording's thread-local is not a plain pointer, constant-initialized")
    # (tests/gpu_trail_off.h: what the fixtures that compile pieces of the renderer alone take for these calls)
    off = (ROOT / "tests" / "gpu_trail_off.h").read_text(encoding="utf-8")
    used = set(re.findall(r"\b(gt_[a-z_]+)\(", "".join(x.read_text(encoding="utf-8") for x in (REPLAY, DRAW, TEXTURE, MEMORY, XFER))))
    for name in sorted(used | {"GtWait", "GtScope", "GtCall", "trail_note"}):
        need(re.search(r"\b%s\b" % name, off), "tests/gpu_trail_off.h has no stand-in for " + name)
    print("PASS: the switch is read once, at the first use; on the console by default; off, a draw leaves on one load and nothing is made", flush=True)


def check_nothing_given(rules, calls, glue, replay):
    for name, text in (("gpu_trail.h", rules), ("vk_gputrail.h", calls), ("vk_gputrail.cpp", glue)):
        code = code_of(text)
        call = re.search(r"\bvk(Cmd|Queue|Wait|Create|Destroy|Allocate|Free|Get|Reset|Begin|End)\w*\(", code)   # (a call: the dump's text may name one)
        need(not call, "%s calls %s: the trail records, it gives the GPU nothing" % (name, call.group(0) if call else ""))
        for never in ("present_mu", "replay_mu", "present_image_mu", "sync_order_mu", "par_mutex"):
            need(never not in code, name + " names " + never + ": the trail takes none of the renderer's locks")
    need('#include "vk_' not in rules and "vulkan" not in rules.lower().replace("no vulkan", ""), "gpu_trail.h is no longer free of Vulkan (the test compiles it alone)")
    tl = block(replay, "void vk_trail_timelines(gt::Now& now)")
    need(tl.count("vkGetSemaphoreCounterValue(dev.device, ") == 3 and "lock" not in tl and "vkWait" not in tl and "queue_flush" not in tl and "completed()" not in tl,
         "the dump's read of the timelines does more than three vkGetSemaphoreCounterValue, or takes a lock")
    for sem in ("timeline", "rec_timeline", "batch_tl"):
        need("if (%s && vkGetSemaphoreCounterValue(dev.device, %s, &v) == VK_SUCCESS)" % (sem, sem) in tl, "the timeline %s is not read, or read when it was not made" % sem)
    need("static_assert(kRecordings <= gt::kSlots" in replay, "nothing checks that every recording slot has its place in the trail")
    # the trail's own lock is never held across a call out of the header: the clock is read before it, the text written after it
    push = block(rules, "uint64_t push(Entry e, Acc* a)")
    need(push.index("e.t_us = clock();") < push.index("lock();") and "clock()" not in push[push.index("lock();"):], "push reads the clock with the lock held")
    back = block(rules, "void back(uint64_t ordinal, int32_t result)")
    need(back.index("const uint64_t t = clock();") < back.index("lock();") and "clock()" not in back[back.index("lock();"):], "back reads the clock with the lock held")
    note = block(rules, "void note(Acc* a, Kind kind, unsigned dim, uint32_t format, uint32_t nvn, uint32_t w, uint32_t h, uint32_t d, uint64_t num)")
    need("clock()" not in note and "recent_us" in note, "a note reads the clock (it takes the newest entry's time)")
    draw = block(rules, "void draw(Acc& a, uint64_t key, uint8_t flags)")
    need("lock()" not in draw and "clock()" not in draw and "a.last >= 0 && a.keys[a.last].key == key ? &a.keys[a.last] : find(a, key, flags)" in draw,
         "a draw takes the lock or reads the clock, or no longer tries the key used last first")
    print("PASS: the trail gives the GPU nothing: no command, no queue call, none of the renderer's locks; a dump reads three counters", flush=True)


def check_queue(replay, texture, memory):
    # every call on the queue in the back end: written into the ring before it, marked when it returns
    excused = {"vk_coopmat.cpp": 1}   # (the cooperative-matrix self-test at start: one dispatch, before any frame; not on the console's driver)
    seen = 0
    for path in sorted(VK.glob("*.cpp")):
        rows = path.read_text(encoding="utf-8").split("\n")
        for i, row in enumerate(rows):
            code = row.split("//")[0]
            if "vkQueueSubmit2(dev.queue" not in code and "vkQueueBindSparse(dev.queue" not in code:
                continue
            if excused.get(path.name, 0) > 0:
                excused[path.name] -= 1
                continue
            seen += 1
            near = "\n".join(rows[max(0, i - 2):i])
            need(re.search(r"const uint64_t trail = (gt_submit\(gt::E_\w+, |trail_scope\.submit\(gt::E_PRESENT, 1\))", near),
                 "%s: a call on the queue is not written into the trail before it: %s" % (path.name, row.strip()))
            # (merged v2.2.0: upstream's device-loss check may come first, as tests/test_present_device_loss.py wants it)
            back = rows[i + 2] if rows[i + 1].strip() == "if (res == VK_ERROR_DEVICE_LOST) device_lost();" else rows[i + 1]
            need(re.match(r"\s*gt_back\(trail, \(int\)(res|submitted)\);", back), "%s: the trail is not told that the call returned: %s" % (path.name, row.strip()))
    # (8 since the Longshot merge: the sparse mirrors' bind, vk_memory.cpp)
    need(seen == 8 and not any(excused.values()), "the back end's calls on the queue changed (%d found): each needs its entry in the trail" % seen)
    submit = block(replay, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0)")
    need("gt_submit(gt::E_REC, gt_slot(r.slot), r.slot, seq, r.value, signal_value ? signal.value : 0, batch_next, n, 0, false);" in submit and
         submit.index("r.batch = batch_next;") < submit.index("gt_submit(gt::E_REC") < submit.index("if (pend.cbs.size() >= cap)"),
         "a recording held for a batch is not written with its batch, before the batch may be sent")
    need("const uint64_t trail = gt_submit(gt::E_REC, gt_slot(r.slot), r.slot, seq, r.value, signal_value ? signal.value : 0, 0, n, 0, true);" in submit,
         "a recording submitted by itself is not written as its own call")
    flush = block(replay, "void queue_flush_locked()")
    need("gt_submit(gt::E_FLUSH, nullptr, 0, pend.rec, 0, pend.any_signal ? pend.signal : 0, batch_next, (unsigned)pend.cbs.size()," in flush and
         flush.index("gt_submit(gt::E_FLUSH") < flush.index("vkQueueSubmit2") < flush.index("gt_back(trail, (int)res);") < flush.index("pend.cbs.clear()"),
         "a batch's flush is not written with its batch, its signals and its command buffers before they are cleared")
    need("gt_end_recording_thread();" in block(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false)"),
         "a parallel replay's worker keeps its slot's recording after its segment")
    present = block(replay, "void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view)")
    # (merged v2.2.0: the present itself is present_out's, after present_image's copy, and upstream's presenter thread has one)
    need(before(replay, "vkQueuePresentKHR(", r"GtCall watched\(\"vkQueuePresentKHR", 1) == 3 and replay.count("vkQueuePresentKHR(") == 3,
         "the present itself is not watched while it is in the driver")
    need(present.index("vkBeginCommandBuffer(f.cmd, &bi)") < present.index("GtScope trail_scope;") < present.index("vkCmdClearColorImage") <
         present.index("trail_scope.submit(gt::E_PRESENT, 1)") < present.index("present_out(index, qk)"),
         "the present's command buffer is not recorded into an entry of its own")
    print("PASS: the %d calls on the queue (recordings, batches, segments, presents, read-backs, sparse binds) are entries, marked when they return" % seen, flush=True)


def check_commands(replay, draw, texture, memory, xfer):
    # clears: every one in the files the game's work goes through, with a kind that says which path it is
    clears = 0
    for name, text in (("vk_replay.cpp", replay), ("vk_draw.cpp", draw), ("vk_texture.cpp", texture), ("vk_memory.cpp", memory), ("vk_xfer.cpp", xfer)):
        for call in ("vkCmdClearColorImage(", "vkCmdClearDepthStencilImage(", "vkCmdClearAttachments("):
            clears += before(text, call, r"(gt_note|gt_note_tex|trail_note)\(gt::(CLEAR_\w+|UP_ZERO_\w+)", 1)
    # (15 since the Longshot merge: the dummy images' clear, vk_draw.cpp, where there is no nullDescriptor)
    need(clears == 15, "the clears changed (%d found): each needs its record call on the line before it" % clears)
    zeros = block(texture, "bool upload_zeros(Texture& tex, uint64_t total)")
    need(zeros.index("gt_note_tex(gt::UP_ZERO_DS, tex);") < zeros.index("vkCmdClearDepthStencilImage") < zeros.index("gt_note_tex(gt::UP_ZERO_COLOR, tex);") <
         zeros.index("vkCmdClearColorImage"), "the zero upload's clears are not recorded as the zero upload's")
    create = block(texture, "Texture* create(const Key& key, const nvn_tex* t, VkFormat vf, bool astc, int bw, int bh, int bpb, bool as_target)")
    need("gt_note_tex(gt::CLEAR_NEW_DS, x);" in create and "gt_note_tex(gt::CLEAR_NEW_COLOR, x);" in create, "a new image's clear is not recorded as a new image's")
    need(draw.count("gt_note(gt::CLEAR_ATT_COLOR, 2, (uint32_t)at.color_format[index], 0, lw, lh, 1);") == 1 and
         draw.count("gt_note(gt::CLEAR_ATT_DS, 2, (uint32_t)at.depth_format, 0, at.width, at.height, 1);") == 1 and
         draw.count("gt_note(gt::CLEAR_MASKED, 2, (uint32_t)at.color_format[index], 0, lw, lh, 1);") == 1,
         "a bound target's clear is not recorded with its format and size")
    need(replay.count("gt_note(gt::CLEAR_SWAP, 2, (uint32_t)dev.swap_format, 0, dev.swap_extent.width, dev.swap_extent.height, 1);") == 3,
         "the swapchain image's clears are not recorded")   # (merged v2.2.0: and blit_crop_to_rect's, an empty source's)
    # uploads, by the path they took
    upload = block(texture, "bool upload_inner(Texture& tex, const Ranges* dirty) {")   # (merged v2.2.0: upload is upload_inner, timed)
    need(before(upload, "vkCmdCopyBufferToImage(upload_cmd()", r"gt_note_tex\(gt::UP_STAGING, tex\);", 1) == 2, "a staging copy is not recorded as one")
    need(before(block(texture, "bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty)"), "vkCmdDispatch(", r"gt_note_tex\(gt::UP_UNSWIZZLE_GPU, tex\);", 1) == 1,
         "the GPU's unswizzle from an imported pool is not recorded")
    # (ps5 lot 60, #1987: a group of a texture's levels is encoded in upload_astc_gpu itself, each dispatch a BC_ENCODE)
    astc = block(texture, "bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty)")
    need(before(astc, "vkCmdDispatch(", r"gt_note_tex\(gt::(UP_ASTC_GPU|BC_ENCODE), tex\);", 1) == 3 and
         before(astc, "vkCmdDispatch(cmd, (g.tw + 7) / 8", r"gt_note_tex\(gt::UP_ASTC_GPU, tex\);", 1) == 2 and
         before(astc, "vkCmdDispatch(cmd, (qp.blocks_x + 7) / 8", r"gt_note_tex\(gt::BC_ENCODE, tex\);", 1) == 1,
         "the GPU's ASTC decoding, or a group's BC3 encoding, is not recorded")
    need(before(block(texture, "void bc_encode_into("), "vkCmdDispatch(", r"gt_note_tex\(gt::BC_ENCODE, dst\);", 1) == 1, "the BC3 encoding is not recorded")
    # the game's copies, blits and clears of textures: every command of vk_xfer.cpp
    n = 0
    for call in ("vkCmdCopyBufferToImage(", "vkCmdCopyImageToBuffer(", "vkCmdCopyImage(", "vkCmdBlitImage(", "vkCmdResolveImage("):
        n += before(xfer, call, r"trail_note\(gt::(COPY_B2T|COPY_T2B|COPY_T2T|BLIT_T2T|RESOLVE_T2T), [sd]\);", 3)
    need(n == 6, "vk_xfer.cpp's copies and blits changed (%d found): each needs its record call" % n)
    need("gt_note(kind, (unsigned)s.tex->type + 1, (uint32_t)s.tex->format, (uint32_t)s.nvn_format, s.w, s.h, s.d);" in block(xfer, "void trail_note(gt::Kind kind, const Sub& s)"),
         "a copy is not recorded with its image's type, its format and the region's size")
    need(replay.index("gt_note_num(gt::COPY_B2B, c.size);") < replay.index("vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &r);") and
         # (since the Longshot merge the game's buffer clear is gpu_fill's: its 4-byte-aligned middle by vkCmdFillBuffer)
         "if (mid) gt_note_num(gt::FILL_BUF, mid), vkCmdFillBuffer(cmd, b.buffer, b.offset + head, mid, w);" in replay,
         "the game's buffer copies and clears are not recorded with their sizes")
    need(before(block(replay, "void blit_crop_to_rect("), "vkCmdBlitImage(", r"gt_note\(gt::BLIT_PRESENT, 2, 0, 0, rect\.w, rect\.h, 1\);", 1) == 1, "the present's blit is not recorded")
    need(before(draw, "vkCmdBlitImage(", r"gt_note_tex\(gt::BLIT_STUB, \*dst\);", 1) == 1, "the upscale stub's blit is not recorded")
    need(memory.index("gt_note_num(gt::MIRROR_SYNC, gp.bytes);") > memory.index("checkpoint_mark(upload_command_buffer(), p.lo, 'm');") and
         memory.index("checkpoint_mark(cmd, p->lo, 'b');") < memory.index("gt_note_num(gt::WRITE_BACK, run_bytes);"), "the mirrors' copies are not recorded where their checkpoints are")
    # (merged v2.2.0: the read-backs are recorded in batches, flush_record, and submitted together, readback_submit_wait)
    need("gt_note_tex_next(gt::READBACK, t);" in block(texture, "bool flush_record(Flush& f, VkCommandBuffer cmd = VK_NULL_HANDLE) {"), "a read-back's copy is not recorded")
    # draws and dispatches, by program
    body = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d)")
    need(body.count("gt_draw(") == 1 and body.index("push_constants(cmd, p, &at);") < body.index("gt_draw(cmd, p.key, !d.mesh ? gt::K_DRAW : p.fallback ? gt::K_FALLBACK : gt::K_MESH);") <
         body.index("dynamic_state(cmd, s, at, topo, d.mesh && !p.fallback);"),
         "a draw is not recorded by its program where its checkpoint is set, with whether it is a mesh shader's or the fallback's")
    disp = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect)")
    need(before(disp, "vkCmdDispatchIndirect(", r"gt_dispatch\(cmd, p\.key, gt::K_DISPATCH \| gt::K_INDIRECT, s\.grid\[0\], s\.grid\[1\], s\.grid\[2\]\);", 1) == 1 and
         before(disp, "vkCmdDispatch(cmd, x, y, z)", r"gt_dispatch\(cmd, p\.key, gt::K_DISPATCH, x, y, z\);", 1) == 1,
         "a dispatch is not recorded by its program with its workgroups")
    fb = block(draw, "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d)")
    # (ps5-next-15, upstream 2611a99: a task program's mesh stage is dispatched indirect from the records' header, so one
    # direct dispatch and two indirect ones)
    need(before(fb, "vkCmdDispatch(", r"gt_dispatch\(cmd, p\.key, gt::K_FB_DISPATCH, (slots|n), 1, 1\);", 1) == 1 and
         before(fb, "vkCmdDispatchIndirect(", r"gt_dispatch\(cmd, p\.key, gt::K_FB_DISPATCH \| gt::K_INDIRECT, 0, 0, 0\);", 1) == 2,
         "the mesh fallback's dispatches are not recorded as the fallback's")
    need(before(draw, "vkCmdDispatch(cmd, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);", r"gt_note\(gt::HOST_DISPATCH, 0, 0, 0, \(size\[0\] \+ 7\) / 8, \(size\[1\] \+ 7\) / 8, 1\);", 1) == 2,
         "the stand-ins' own dispatches are not recorded")
    begin = block(draw, "void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value)")
    need("gt_begin_recording(slot, cmd);" in begin and begin.index("gt_begin_recording(slot, cmd);") < begin.index("ParLock lk;"),
         "a recording's start does not give its command buffer and its thread the slot's place in the trail")
    bind = block(GLUE.read_text(encoding="utf-8"), "void gt_begin_recording(int slot, const void* cmd)")
    need("a->reset();" in bind and "if (gt_trail->by_cmd(cmd) != a) gt_trail->bind_cmd(cmd, a);" in bind and "gt_acc = a;" in bind,
         "a recording's start no longer empties its slot's place, binds its command buffer to it and gives it to the thread")
    # imports and images
    imp = block(memory, "bool import_pool(uint64_t lo, uint64_t hi, Pool& p)")
    need(imp.count("gt_note_num(gt::IMPORT, bytes);") == 2 and imp.count("gt_note_num(gt::IMPORT_REFUSED, bytes);") == 2,
         "an import made, or refused by the window or the driver, is not recorded")
    need(memory.count("gt_note_num(gt::IMPORT_BACK, ") == 3 and "gt_note_num(gt::IMPORT_BACK, dead_imports.front().bytes);" in block(memory, "void reap_mirrors()"),
         "an import given back is not recorded")
    # (the dead list's walk is reap_dead since ps5-next-22: the present's, and the eviction pass's own, RX_VK_EVICT_FREE_NOW)
    frame = block(texture, "size_t reap_dead(uint64_t completed, uint64_t hold, uint64_t* bytes = nullptr)")
    need(frame.index("gt_note_num(gt::IMG_DESTROY, it->value);") < frame.index("destroy_image(it->image, it->allocation);") and
         frame.index("gt_note_num(gt::IMG_POOLED, it->value);") < frame.index("pool_put(*it);"), "a destroyed image is not recorded with the last submission that used it")
    need("reap_dead(completed, hold);" in block(texture, "void texture_frame(uint64_t completed)"),
         "the present no longer reaps the dead images through reap_dead")
    kinds = set()
    for text in (replay, draw, texture, memory, xfer):
        for m in NOTE.finditer(text):
            if m.group(2).isupper():   # (not trail_note's own "gt::Kind kind")
                kinds.add(m.group(2))
    rules = RULES.read_text(encoding="utf-8")
    listed = re.findall(r"^    ([A-Z][A-Z0-9_]+),", rules[rules.index("enum Kind : uint8_t {"):rules.index("    KINDS\n")], re.M)
    need(len(listed) == 34 and sorted(kinds) == sorted(listed), "kinds no renderer file records, or recorded and not listed: %s" %
         sorted(set(listed) ^ kinds))
    print("PASS: %d clears, the uploads by their path, the copies and blits, draws and dispatches by program, imports and images: each has its record call; "
          "%d kinds, all used" % (clears, len(listed)), flush=True)


def check_waits(calls, rules, replay, texture, memory):
    waits = (
        (replay, 'GtWait watched("a recording slot\'s batch", r.batch);', "vkWaitSemaphores(dev.device, &wi, UINT64_MAX);"),
        (replay, 'GtWait watched("a recording slot\'s own fence, submission value", r.value);', "vkWaitForFences(dev.device, 1, &r.fence, VK_TRUE, UINT64_MAX);"),
        (replay, 'GtWait watched("the device to be idle, batch", batch_flushed);', "vkDeviceWaitIdle(dev.device);"),
        (replay, 'GtWait watched("submission value", v);', "vkWaitSemaphores(dev.device, &wi, UINT64_MAX);"),
        (replay, 'GtWait watched("the game\'s fence, recording", seq);', "res = vkWaitSemaphores(dev.device, &wi, timeout_ns);"),   # (#670 keeps the VkResult)
        (replay, 'GtWait watched("the game\'s fence, submission value", f);', "res = vkWaitSemaphores(dev.device, &wi, timeout_ns);"),
        (replay, 'GtWait watched("an earlier present\'s own fence, present slot", (uint64_t)log_slot);', "if (vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX) == VK_ERROR_DEVICE_LOST) device_lost();"),
        (texture, 'GtWait watched("a texture read-back\'s fence", 0);', "const VkResult r = vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull);"),   # (the merge: its result kept, rb_pending)
        (texture, 'GtWait watched("a sparse bind\'s fence", 0);', "vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);"),
        (memory, 'GtWait watched("a pool\'s sparse bind\'s fence", 0);', "res = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);"),
    )
    for text, watch, wait in waits:
        need(re.search(re.escape(watch) + r"\n\s*" + re.escape(wait), text), "a wait is no longer watched, or no longer the wait it was: " + wait)
    need(sum(t.count("GtWait watched(") for t in (replay, texture, memory)) == len(waits), "a watched wait the test does not know")
    w = block(rules, "struct Waits")
    for never in ("lock()", "sleep", "yield", "wait(", "clock"):
        need(never not in code_of(w), "watching a wait can hold the waiting thread: " + never)
    gw = block(calls, "struct GtWait")
    need("vk" not in gw and "gt_wait_begin(what, value)" in gw and "if (slot >= 0) gt_wait_end(slot);" in gw, "GtWait does more than mark a wait's start and end")
    print("PASS: the %d waits for a submission are watched and are the waits they were, timeouts and all" % len(waits), flush=True)


def check_dump(rules, glue, internal, host, early, posix):
    line = block(glue, "void trail_line(const char* line)")
    need(re.search(r'#ifdef __PROSPERO__\s+hos_ps5_trail\("%s", line\);\s+#else\s+logf\("%s", line\);\s+#endif', line),
         "a dump's lines do not go to the stage trail on the console (the queue's lock line's path) and the log elsewhere")
    dump = block(glue, "void gpu_trail_dump(const char* why)")
    need(dump.index("if (dumping.exchange(true, std::memory_order_acquire)) return;") < dump.index("gt_trail->written_since_dump(&written, &call)") <
         dump.index('"gpu trail: d%u begins at %.3f s') < dump.index("trail_line(line);\n    }\n    gt::Now now;") < dump.index("vk_trail_timelines(now);") <
         dump.index("gt_trail->dump(now, text);") < dump.index("gt::emit(text, whole, trail_line);"),
         "a dump no longer writes its first line before the driver is asked, or its text after the trail's lock is let go")
    need('if (!news && !gt_trail->moved(now)) return again("nothing was submitted and no timeline has moved since");' in dump and
         "if (whole >= kDumpsMost)" in dump and "const bool soon = whole && t - whole_at < kDumpGapUs;" in dump and
         'if (news && soon) return again("not written again so soon after");' in dump and "if (repeats++ >= kRepeatsMost) return;" in dump,
         "a dump is written again for the same stall, or without end")
    # (2026-11-11) the watchdog's question is kept from the thread that writes a dump, from before its first line to its return.
    # Why: the host asks with SIGUSR2, the handler answers with a stage-trail line, and that trail's lock is a spinlock held
    # across a line's write and sync: a thread asked inside one of its own lines would wait for itself for good, the lock held
    held = block(glue, "struct AskHeld")
    need("#if defined(__PROSPERO__) || defined(__linux__)" in held and "sigfillset(&ask);" in held and "if (s != SIGUSR2) sigdelset(&ask, s);" in held and
         "held = pthread_sigmask(SIG_BLOCK, &ask, &before) == 0;" in held and "if (held) pthread_sigmask(SIG_SETMASK, &before, nullptr);" in held,
         "the thread that writes a dump no longer blocks the watchdog's SIGUSR2, and it alone, while it writes, or no longer gives its mask back")
    # (with calls the eboot on the console makes already, at the driver's thread starts: an import it has never resolved is a new way
    # for a launch to fail)
    need(not re.search(r"\bsig(addset|emptyset|action|procmask|wait|suspend)\(|\bsignal\(", code_of(glue)),
         "vk_gputrail.cpp calls a signal function other than sigfillset, sigdelset and pthread_sigmask")
    need(dump.index("} done;") < dump.index("AskHeld ask_held;") < dump.index("trail_line("),
         "a dump writes a line before the watchdog's question is held back")
    ask = block(posix, "int hos_posix_ask_where(const char *prefix)")
    need("pthread_kill(t->th, SIGUSR2)" in ask and "static void on_quiet_signal(int sig, siginfo_t *si, void *uctx)" in posix and
         'hos_ps5_trail("crash: %s: signal %d' in block(posix, "static void ps5_trail_signal("),
         "the watchdog no longer asks with SIGUSR2, or its handler no longer writes to the stage trail: what a dump's thread blocks is to follow")
    need("static volatile int trail_busy;" in early and "while (__atomic_exchange_n(&trail_busy, 1, __ATOMIC_ACQUIRE))" in block(early, "static void trail_lock(void)"),
         "the stage trail's lock changed: whether a thread may be asked inside one of its lines is to be looked at again")
    # the three places that ask
    lock = block(internal, "__attribute__((noinline)) void lock()")
    need(lock.index("if (told++) continue;") < lock.index("has waited 5 s") < lock.index('gpu_trail_dump("the queue\'s lock has been waited for 5 s");') and
         "void gpu_trail_dump(const char* why);" in internal[:internal.index("struct PresentMutex")],
         "the queue's lock no longer has the trail written when it has been waited for 5 s, once a wait, after its own line")
    report = block(host, "static void sched_report(")
    need(report.index('hos_posix_ask_where("sead::MainThread");') < report.index('if (rx_ps5_vk_gpu_trail_dump) rx_ps5_vk_gpu_trail_dump("no frame for 20 s");') <
         report.rindex("still = 0;") and "void rx_ps5_vk_gpu_trail_dump(const char *why) __attribute__((weak));" in report and
         report.index("if (still >= 2 && asked < 2) {") < report.index("rx_ps5_vk_gpu_trail_dump(\"no frame for 20 s\")"),
         "the host's watchdog no longer asks the renderer for its trail at no frame for 20 s (weakly: a title with no Vulkan renderer links)")
    watch = block(host, "static DWORD WINAPI sched_watch_main(void *p)")
    need("void rx_ps5_vk_gpu_trail_poll(void) __attribute__((weak));" in watch and
         "if (rx_ps5_vk_gpu_trail_poll && now - trail_ticked >= 1000000000LL) {" in watch and "rx_ps5_vk_gpu_trail_poll();" in watch,
         "the host's report thread no longer ticks the trail's watcher once a second")
    need('extern "C" void rx_ps5_vk_gpu_trail_poll(void) { mkwvk::gt_poll(); }' in glue and
         'extern "C" void rx_ps5_vk_gpu_trail_dump(const char* why) { mkwvk::gpu_trail_dump(why && *why ? why : "asked"); }' in glue,
         "the renderer no longer has the two functions the host calls")
    poll = block(glue, "void gt_poll()")
    need(poll.index("waits.advance();") < poll.index("waits.overdue(kWaitTicks)") < poll.index("gpu_trail_dump(why);") and "constexpr uint32_t kWaitTicks = 6;" in glue,
         "the watcher no longer has the trail written for a wait that has seen six of its ticks (5 s at least, a tick a second)")
    # a line of the dump fits the stage trail's line with its stamp: 1,024 bytes, "[%8.3f] " first (host/posix/ps5_early.c vtrail)
    vtrail = block(early, "static void vtrail(const char *fmt, va_list ap)")
    need("char line[1024];" in vtrail and '"[%8.3f] "' in vtrail, "the stage trail's line changed: the dump's lines are sized for 1,024 bytes with its stamp")
    kline = int(re.search(r"constexpr int kLine = (\d+);", rules).group(1))
    need(kline + len("gpu trail: d99 999/999: ") + len("[12345.678] ") + 2 <= 1000, "a dump line with its prefix and the trail's stamp is over 1,000 characters")
    need('char out[kLine + 64];' in block(rules, "inline void emit(const Text& t, unsigned id, Sink&& sink)") and
         '"gpu trail: d%u %d/%d: %.*s"' in rules, "a dump's line no longer says which dump it is of and where in it")
    for l in re.findall(r'"gpu trail: [^"]*"', glue):
        need(len(l) < 300, "one of the dump's own lines is long: " + l[:60])
    need("char line[640];" in dump, "the dump's own lines are no longer built in 640 bytes")
    print("PASS: the dump goes out by the stage trail from the lock's wait, the watcher and the watchdog; first line before the driver is asked; "
          "once a stall; its thread not asked where it is meanwhile; lines of %d characters at most" % (kline + len("gpu trail: d99 999/999: ")), flush=True)


def vulkan_headers(given):
    """The Vulkan headers' include folder: the argument, HOOKSHOT_VULKAN_HEADERS, or the CPM cache nx2/build.py reads; None: not here"""
    for d in (given, os.environ.get("HOOKSHOT_VULKAN_HEADERS")):
        if d and (Path(d) / "vulkan" / "vulkan.h").is_file():
            return Path(d)
    for cpm in (os.environ.get("RX_CPM"), ROOT / "build-linux" / "deps" / "cpm"):
        d = Path(cpm) / "vulkanheaders" if cpm else None
        if d and d.is_dir() and any(d.iterdir()):
            inc = sorted(d.iterdir())[-1] / "include"
            if (inc / "vulkan" / "vulkan.h").is_file():
                return inc
    return None


def check_glue(cxx, headers):
    """vk_gputrail.cpp itself, off the console (its log for the stage trail, a fake for the driver's timelines): the three settings of
    the switch"""
    inc = vulkan_headers(headers)
    if os.name == "nt" or not inc or not shutil.which(cxx):
        print("skipped: the glue's own run (gpu/vk/vk_gputrail.cpp needs the Vulkan headers and %s; POSIX)" % cxx, flush=True)
        return
    with tempfile.TemporaryDirectory(prefix="gpu-trail-glue-") as tmp:
        exe = Path(tmp) / "glue"
        subprocess.run([cxx, "-std=c++20", "-O1", "-pthread", "-I" + str(ROOT / "host" / "posix"), "-D_GNU_SOURCE", "-DVK_NO_PROTOTYPES",
                        "-I" + str(VK / "compat"), "-I" + str(ROOT / "gpu" / "compat"), "-I" + str(inc), "-I" + str(VK),
                        "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring", "-include", "functional",
                        "-include", "string", "-include", "vector",
                        str(ROOT / "tests" / "gpu_trail_glue_test.cpp"), str(GLUE), "-o", str(exe)], check=True)
        checks = 0
        for setting, arg in ((None, []), ("0", []), ("1", ["on"]), ("1", ["on", "at"])):
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_GPU_TRAIL", "RX_VK_GPU_TRAIL_AT")}
            if setting is not None:
                env["RX_VK_GPU_TRAIL"] = setting
            if "at" in arg:
                env["RX_VK_GPU_TRAIL_AT"] = "0.05"
            r = subprocess.run([str(exe)] + arg, env=env, capture_output=True, text=True, timeout=120)
            out = r.stdout + r.stderr
            if r.returncode or "FAIL " in out or (arg and "PASSED: 0 failed" not in out) or (not arg and "PASS off: no line is written" not in out):
                print(out[-4000:])
                raise SystemExit("FAIL: the glue's run with RX_VK_GPU_TRAIL %s (exit %d)" % ("unset" if setting is None else "=" + setting, r.returncode))
            checks += out.count("PASS ")
    print("PASS: gpu/vk/vk_gputrail.cpp run off the console: unset and 0, nothing made and nothing written; 1, the dump once a stall, "
          "the watchdog's question kept until it is written, the watched wait, a present's own entry; RX_VK_GPU_TRAIL_AT, one dump at that "
          "time (%d checks)" % checks, flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--glue-cxx", default="clang++")
    p.add_argument("--headers", default=None)
    p.add_argument("--bench", action="store_true")
    a = p.parse_args()
    check_rules(a.cxx, a.bench)
    rules, calls, glue = (x.read_text(encoding="utf-8") for x in (RULES, CALLS, GLUE))
    replay, draw, texture, memory, xfer = (x.read_text(encoding="utf-8") for x in (REPLAY, DRAW, TEXTURE, MEMORY, XFER))
    check_switch(rules, calls, glue)
    check_nothing_given(rules, calls, glue, replay)
    check_queue(replay, texture, memory)
    check_commands(replay, draw, texture, memory, xfer)
    check_waits(calls, rules, replay, texture, memory)
    check_dump(rules, glue, INTERNAL.read_text(encoding="utf-8"), HOST.read_text(encoding="utf-8"), EARLY.read_text(encoding="utf-8"),
               POSIX.read_text(encoding="utf-8"))
    check_glue(a.glue_cxx, a.headers)


if __name__ == "__main__":
    main()
