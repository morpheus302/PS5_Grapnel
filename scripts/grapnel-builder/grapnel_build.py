#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel builder v0.2.3 (Linux and Windows, command line): the PS5 title GRPN00001 made on the player's PC from their own game file.

    python3 -I scripts/grapnel-builder/grapnel_build.py --nup <game.nup> --out <folder> --toolchain <folder> [options]
    python3 -I scripts/grapnel-builder/grapnel_build.py --check --toolchain <folder> [options]     (the tools alone, no game)

The eboot holds the game's code translated to C and compiled, image.bin is the game's program as the console loads it, and the
button icons hold the game's font and textures: none of them may be shared, so every player makes them here, from their own
copy. Nothing written under --out or --work may leave the player's machine, except towards their own console.

What it needs (INSTALL.md beside this file):
  - the game: the .nup of The Legend of Zelda: Ocarina of Time, Nintendo Switch 2 Edition, with its update v131072 (title
    0400486024DEC800, or the base game's 0400486024DEC000 in a dump that keeps the update under it), the console dump the PS5
    port reads as game.nup. Nothing else is taken (no unpacked folders);
  - LLVM 19.1.7 (clang, clang++, lld, llvm-ar/nm/objcopy, compiler-rt's builtins): the compiler every Grapnel eboot is made with,
    so that every player makes the same one (--llvm, else llvm-config-19 / llvm-config on PATH, else /usr/lib/llvm-19);
  - the console toolchain folder (--toolchain: make_toolchain.py makes it): the PS5 payload SDK, PS5_Vulkan's link recipe and
    ps5-native-tool, the RADV archive, the renderer's headers, dav1d. Without it, the places the port's own PC has them;
  - Python 3.12+ with requirements.txt (numpy, capstone, lz4, zstandard; the icons' fontTools, Pillow, astc-encoder-py,
    texture2ddecoder).

The steps (grapnel_core.py, which a window can drive the same way):
  1. the .nup checked (a NUP package, the supported game and update) and the game's program read out of it (nx2/nup.py: about
     40 MB; ZBIC unpacked by nx2/zbic_unpack, a program of its own), with the 33 romfs files the button icons are made from;
  2. translated to C (nx2/lift.py --standalone, nx2/split_hot.py) with the port's lift settings and the address lists in
     profile/ (which functions are hot, where the C files are cut: addresses only, no game code);
  3. compiled for the console and linked (nx2/build.py --standalone, RX_TARGET=ps5), which signs the eboot and lays out the title
     folder; the flexible memory is 580 MiB, checked against the eboot's segments; the imports and the shader stamp are checked;
  4. the folders put together: homebrew/GRPN00001 (with the title's images from art/ or --title-art) and data/grapnel
     (image.bin, the settings file and skip.txt from data/, the launch screen's background when the art folder has one);
  5. the button icons made from the game's own files (nx2/prompt_assets.py), into data/grapnel/prompts;
  6. the lists the shaders' preparation reads, in the work folder: shaders/hookshot_shaders.bin, the census of the game's
     shaders (nx2/shader_census.py on the ~290 MB of the romfs it reads, out of the .nup; PRIVATE: the shaders' code), and
     shaders/hookshot_pipeline_states_shipped.bin, the pipelines to build (this checkout's assets/pipeline_states merged by
     nx2/pipeline_states.py, less the lists converted from another emulator's records: no game file). --no-shaders: neither,
     and no step 7;
  7. (lot 83) the shaders prepared on this PC: the toolchain's prepare/ (make_prepare_tool.py: no game code) runs the console's
     own preparation (lot 82) on every core, on the PC model of the console's driver, into the driver's cache as the console
     keeps it; checked (every part whole, the progress file finished, and a second pass that compiles nothing), then laid in
     homebrew/GRPN00001/radv-shader-cache (the console's /app0/radv-shader-cache) with its progress file, which the launch
     screen reads (also data/grapnel/hookshot_prepare_progress.txt). --no-prepare: not made (the console compiles in play);
  8. INSTALL (fr, en), PRIVATE.txt and manifest.json written.

Output for a window: lines starting with "@@" (the rest is the tools' log, indented, and notes for the player):
    @@step <n> <of> <name>     @@progress <0..1|-> <text>     @@done <folder>     @@error <code> <message>

The shaders' preparation is the console's own code (gpu/vk/vk_draw.cpp console_prepare_*, gpu/vk/console_prepare.h), run here
on the PC; the console only reads the cache it makes (the launch screen's shader check). It needs Linux x86-64 (the
Windows builder gets a native port of it later) and the toolchain's prepare/ of this release (the same shader stamp).

Ideas taken from heydemoura's Longshot PS5 Builder (heydemoura/PS5_Longshot, ps5/builder/pipeline.py, GPL-3.0): the numbered
steps with "@@" lines for a window, a memory budget for the build, the check of the tools with no game. Its code is not copied:
this drives the port's own nx2/ tools.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import grapnel_core as core  # noqa: E402


class PrintEvents(core.Events):
    """The command line's view: "@@" lines for a window or a script, the rest for a person"""

    def __init__(self):
        self.last = None

    def step(self, index, total, name):
        print("@@step %d %d %s" % (index, total, name), flush=True)
        self.last = None

    def progress(self, fraction, text=""):
        shown = "-" if fraction is None else "%.2f" % fraction
        if shown != self.last:   # (at most a line a percent)
            print("@@progress %s %s" % (shown, text), flush=True)
            self.last = shown

    def log(self, line):
        print("   " + line, flush=True)

    def info(self, text):
        print(text, flush=True)

    def step_done(self, index, seconds, peak_mib):
        print("   (%s: %.0f s%s)" % (core.STEPS[index - 1], seconds, ", peak %.0f MiB" % peak_mib if peak_mib is not None else ""),
              flush=True)


class JsonEvents(core.Events):
    """(grapnel, the window) every event as one line "@@json {...}" (UTF-8): grapnel_gui.py reads them from this process's output"""

    def __init__(self):
        self.last = None
        self.out = sys.stdout

    def emit(self, **kw):
        print("@@json " + json.dumps(kw, ensure_ascii=False), file=self.out, flush=True)

    def step(self, index, total, name):
        self.last = None
        self.emit(e="step", index=index, total=total, name=name)

    def progress(self, fraction, text=""):
        shown = None if fraction is None else round(fraction, 3)
        if shown != self.last:
            self.last = shown
            self.emit(e="progress", fraction=shown, text=text)

    def log(self, line):
        self.emit(e="log", line=line)

    def info(self, text):
        self.emit(e="info", text=text)

    def step_done(self, index, seconds, peak_mib):
        self.emit(e="step_done", index=index, seconds=round(seconds, 1), peak_mib=peak_mib)


def main():
    ap = argparse.ArgumentParser(description="Grapnel builder %s: the PS5 title GRPN00001 from your own game's .nup" % core.VERSION)
    ap.add_argument("--check", action="store_true", help="check the tools on this PC (no game needed), and stop")
    ap.add_argument("--nup", help="your game file: the .nup of Zelda: Ocarina of Time (Switch 2 Edition) with its update v131072")
    ap.add_argument("--out", help="where the title and data folders go")
    ap.add_argument("--work", help="the build's own files (default <out>/grapnel-work; kept, so a second build is short)")
    ap.add_argument("--toolchain", help="the unpacked grapnel-toolchain folder (default: the port's own PC's places)")
    ap.add_argument("--llvm", help="LLVM %s's folder or bin folder (default: llvm-config-19 / llvm-config on PATH, /usr/lib/llvm-19)"
                    % core.LLVM_VERSION)
    ap.add_argument("--jobs", type=int, default=0, help="compiles at once (default: the cores less one; each starts when the memory allows)")
    ap.add_argument("--lift-jobs", type=int, default=0, help="the translation's processes (default: by the memory free, 1 to 8)")
    ap.add_argument("--memory", type=float, default=0, help="GB the compile may use (default: the memory free decides)")
    ap.add_argument("--python", default=sys.executable, help="the Python for the translation and the build (requirements.txt)")
    ap.add_argument("--prompt-python", help="the Python for the button icons (default --python)")
    ap.add_argument("--no-prompts", action="store_true", help="no button icons (the game shows the Switch's)")
    ap.add_argument("--title-art", help="a folder with icon0.png, pic0.dds (pic1.dds: the same, or its own), hookshot_menu_fond.rgba or .png (default: art/ here)")
    ap.add_argument("--lock", help="take an exclusive lock on this file while translating and compiling")
    ap.add_argument("--nice", type=int, default=0, help="run at this niceness (the tools inherit it)")
    ap.add_argument("--measure", action="store_true", help="sample the build's memory (PSS of every process, Linux) and say each step's peak")
    ap.add_argument("--events", choices=("text", "json"), default="text",
                    help="json: every event as an @@json line, for the window (grapnel_gui.py)")
    ap.add_argument("--no-shaders", action="store_true", help="no list of the game's shaders and no preparation: the game compiles "
                    "them as it plays")
    ap.add_argument("--no-prepare", action="store_true", help="the lists, but no preparation on this PC: the game compiles the shaders "
                    "as it plays")
    ap.add_argument("--prepare-workers", type=int, default=0, help="the preparation's compiles at once (default: every core; the memory "
                    "free decides how many work)")
    a = ap.parse_args()
    # (the tools run from the checkout's root: a relative path given here is the caller's, made absolute now)
    for k in ("python", "prompt_python", "nup", "out", "work", "toolchain", "llvm", "title_art", "lock"):
        v = getattr(a, k)
        if v and (k not in ("python", "prompt_python") or os.sep in v):
            setattr(a, k, os.path.abspath(v))
    if not a.check and not (a.nup and a.out):
        ap.error("--nup and --out are needed (or --check)")
    if a.events == "json":   # (UTF-8 whatever the system's code page: a player's folder may have accents)
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    if a.nice and hasattr(os, "nice"):
        os.nice(a.nice)
    options = core.Options(nup=a.nup or "", out=a.out or "", work=a.work or "", toolchain=a.toolchain or "", llvm=a.llvm or "",
                           jobs=a.jobs, lift_jobs=a.lift_jobs, memory=a.memory, python=a.python, prompt_python=a.prompt_python or "",
                           no_prompts=a.no_prompts, title_art=a.title_art or "", lock=a.lock or "", measure=a.measure,
                           no_shaders=a.no_shaders, no_prepare=a.no_prepare, prepare_workers=a.prepare_workers)
    events = JsonEvents() if a.events == "json" else PrintEvents()
    builder = core.Builder(options, events)
    try:
        if a.check:
            builder.check()
            if a.nup:   # (with --nup, the game file too: the window's "Check")
                tid, ver = core.check_nup(a.nup)
                builder.ev.info("ok: the .nup holds %016X v%d, the supported game and update" % (tid, ver))
            if a.events == "json":
                events.emit(e="done", what="check")
            else:
                print("@@done check", flush=True)
        else:
            builder.run()
            if a.events == "json":
                events.emit(e="done", what="build", out=builder.o.out, times=builder.times)
                return 0
            for s, sec, peak in builder.times:
                print("   %-45s %6.0f s%s" % (s, sec, "   peak %6.0f MiB" % peak if peak is not None else ""), flush=True)
            print("@@done " + builder.o.out, flush=True)
    except core.BuildError as e:
        if a.events == "json":
            events.emit(e="error", code=e.code, message=e.message, details=e.details, log=e.log)
            return 1
        print("@@error %s %s" % (e.code, e.message.replace("\n", " | ")), flush=True)
        print(e.message, file=sys.stderr)
        if e.details:
            print(e.details, file=sys.stderr)
        if e.log:
            print("(the whole log: %s)" % e.log, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
