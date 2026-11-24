# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel builder's core: the PS5 title GRPN00001 made from the player's own game file. No printing, no argument parsing: a
front end (grapnel_build.py, the command line; a window later) gives it Options and an Events sink, and calls the steps.

    from grapnel_core import Builder, Options, Events, BuildError
    b = Builder(Options(nup=..., out=..., toolchain=...), events)
    b.check()            # the tools, no game: LLVM 19.1.7, the console toolchain, Python's packages, the ZBIC program
    b.read()             # 1. the .nup checked, the game's program and the icons' files read out of it
    b.translate()        # 2. the program translated to C
    b.compile()          # 3. compiled, linked, signed: the title folder in the work folder; imports, stamp, memory checked
    b.lay_out()          # 4. out/homebrew/GRPN00001 and out/data/grapnel
    b.icons()            # 5. the button icons from the game's own files
    b.shaders()          # 6. the list of the game's shaders and of the pipelines to build (in the work folder)
    b.prepare()          # 7. the console driver's shader cache made on this PC from them: out/homebrew/GRPN00001/radv-shader-cache
    b.write_notes()      # 8. INSTALL (fr, en), PRIVATE.txt, manifest.json
    b.run()              # all of the above, in order

Each step needs the ones before it on the same Builder (it keeps what they found); a step can be called again after a failure.
What it says goes to the Events sink:
    events.step(index, total, name)      a step begins (1-based)
    events.progress(fraction, text)      how far the step is, 0..1 (None: not known), with a short text
    events.log(line)                     a line of the tools' output (for a log view)
    events.info(text)                    a line for the player (a check passed, a finding)
    events.step_done(index, seconds, peak_mib)
A failure is a BuildError: .code (a short stable name a window can translate or act on), .message (one paragraph for the
player, English), .details (the log's last lines, or ''), .log (the log file, or None).

The core needs no shell: it runs Python and the toolchain's programs directly. (The console toolchain itself still runs bash
scripts in v0.1: the payload SDK's prospero-* wrappers and PS5_Vulkan's tools/radv-link.sh, through nx2/build.py. v0.2 runs
the LLVM tools directly with the toolchain's ps5_recipe.json, on Windows and Linux: nx2/toolchain_ps5.py.) Linux-only parts are optional: the memory sampler (/proc),
the --lock file lock (fcntl), the niceness.

What the build needs, and the steps' details: grapnel_build.py's docstring and INSTALL.md beside this file.
"""
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import struct
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))   # (abspath, not realpath: a checkout reached through a link keeps that path)
ROOT = os.path.dirname(os.path.dirname(HERE))
NX2 = os.path.join(ROOT, "nx2")
PROFILE = os.path.join(HERE, "profile")
ART = os.path.join(HERE, "art")
DATA = os.path.join(HERE, "data")
if NX2 not in sys.path:
    sys.path.insert(0, NX2)
import toolchain_ps5  # noqa: E402  (only os and subprocess: where the SDK and PS5_Vulkan are, the title's id and name)

VERSION = "0.2"
STEPS = ["Reading the game's program", "Translating it to C", "Compiling and linking for the console",
         "Putting the title and data folders together", "Making the button icons",
         "Listing the game's shaders", "Preparing the shaders on this PC", "Writing the notes"]
# The one game and update Grapnel is made from: the newest title of the player's .nup must be this one, and its main this build
GAME_TITLE_ID, GAME_VERSION = 0x0400486024DEC800, 131072
GAME_NAME = "The Legend of Zelda: Ocarina of Time (Nintendo Switch 2 Edition), update v131072"
LLVM_VERSION = "19.1.7"
# The lift's settings: those of the console port's builds since lot 61 (build/lot61/ps5-lift.sh: make_game.py's lift with the
# hot list, A64_FASTFP=0, and Triforce's two lifter changes, A64_FPX_NC=1 A64_VFPX_ELEM=hot). Every one changes the C written
LIFT_ENV = {"A64_HOT_CHUNK": "1", "A64_FASTFP": "0", "A64_FPX_NC": "1", "A64_VFPX_ELEM": "hot"}
FLEXIBLE_MB = 580          # (sce_sys/param.json kernel.flexibleMemorySize, fixed: the value the port runs on the console. A wrong
                           # one can power the console off: only the project changes it, after a run on a console)
FLEXIBLE_AT_MAIN_MB = 5.4  # (what the console takes from flexible memory beside the eboot's segments before main: libc.prx, the
                           # main thread's stack, the system modules; measured 09/11/2026, docs/ps5-port.md)
FLEXIBLE_FLOOR_MB = 48     # (the memory watch closes the title under this much free flexible memory: host/hos_thread.c)
PROMPT_FILES = ("Pack/ZsDic.pack.zs", "Font2/Scalable/", "UI/Layout2Archive/Common.Product.100.",
                "System/Resource/ResourceSizeTable.")   # (what nx2/prompt_assets.py reads of the romfs: ~90 MB of 26 GB)
LIFT_MODULES = ("numpy", "capstone", "lz4", "zstandard")   # (unicorn, in requirements.txt, is the tests' alone)
PROMPT_MODULES = ("fontTools", "PIL", "astc_encoder", "texture2ddecoder", "zstandard")
TITLE_ART = ("icon0.png", "pic0.dds", "pic1.dds")          # (the title folder's sce_sys)
DATA_ART = ("hookshot_menu_fond.rgba",)                    # (the launch screen's background, in the data folder)
# (art/ keeps one home-screen picture, pic0.dds, put in sce_sys as pic1.dds too (the console wants both, the same picture), and
# the launch screen's background as a PNG, made into hookshot_menu_fond.rgba here with its panel on the picture's right side)
MENU_PNG, MENU_PANEL = "hookshot_menu_fond.png", "55,8,92,97"
# (build.py's last line: the title folder made, its eboot's size; the folder may have spaces, as a Windows user's C:\Users\<name>)
TITLE_LINE = re.compile(r"==> (\S+) \(game\): (.+?), eboot\.bin (\d+) bytes")
FONT_DATE = "1767225600"   # (2026-01-01 00:00 UTC: the date written in the button icons' font, the same for every build)
# (lot 82) The console's own shader preparation (gpu/vk/console_prepare.h, the launch screen's Shaders page, HOOKSHOT_PS5_PREPARE=1)
# The shaders' preparation (lot 82: the console's own, gpu/vk/vk_draw.cpp console_prepare_*; lot 83: run on the player's PC by the
# builder, the console only reads the cache it makes) reads two files, made here in the work folder:
#  - <prefix>_shaders.bin, the census of the game's shaders (nx2/shader_census.py; gpu/vk/vk_shader.cpp seed_path): the code of
#    every shader stage of the game, made from the player's own romfs. PRIVATE, like image.bin;
#  - <prefix>_pipeline_states_shipped.bin, the pipelines to build (gpu/vk/vk_draw.cpp state_list_path): Longshot's public lists
#    (assets/pipeline_states/, hashes, formats and draw state: no game file) merged by nx2/pipeline_states.py, less the lists
#    converted from another emulator's records (none of their pipelines was ever drawn by the port: 2 of the 4,558 programs our
#    console drew are in them alone; they are a fifth of the work). Made from this checkout alone.
CENSUS_FOLDERS = ("Shader/", "Layout2Shader/", "Lib/", "Movie/", "Effect/")   # (what nx2/shader_census.py walks: archives())
CENSUS_DICT = "Pack/ZsDic.pack.zs"                                           # (and the game's zstd dictionaries)
STATES_LEFT_OUT = ("pmwork-personal-fritchii-recipe",)
# (this game's census, nx2/shader_census.py on update v131072's romfs: what a census made here must hold)
CENSUS_PROGRAMS, CENSUS_MESH, CENSUS_COMPUTE = 38809, 1312, 4128
# The preparation on the PC (step 7): the toolchain's prepare/ (make_prepare_tool.py: the console tree's Linux host with no game
# code, this release's renderer for Linux, our PS5 RADV built for Linux on the PS5 winsys's host model) runs the console's own
# preparation (`longshot --prepare-shaders`, RX_VK_CONSOLE_PREPARE_RUN=1) on every core, into a Mesa cache database as the
# console's driver keeps it (MESA_DISK_CACHE_DATABASE, 50 parts in 4 GiB): the same keys as the console's driver, whose build ID
# this is (the console's RADV archive and its host model are built with it: mesa-work/build-radv-game.sh, build-host-game.sh)
CONSOLE_DRIVER_BUILD_ID = "1599cbb5b1a0de5dc9beb4d7026883a49e9b5b63"
# the public settings' lines the PC's run leaves out: the console's platform and host (the shader cache's place is the run's)
PREP_PLATFORM_ONLY = ("HOS_VCORES", "HOS_TSC_CLOCK", "RX_VK_SKIP_FILE", "MESA_SHADER_CACHE_DIR")
# (and nothing more: what the console's build has by default and a PC build not, the networks' matrix multiplies looped, the
# toolchain's renderer has too (vk_coopmat.cpp, HOOKSHOT_CACHE_BUILD): RX_VK_MMA_UNROLL=0 here would be in the PC's program stamp,
# which walks RX_VK_MMA* off the console, and the console would take the preparation for another translator's)
PREP_CONSOLE_DEFAULTS = {}
PREP_TRIES = 6           # (runs that stop short of memory, or on a compile that never ends, go on where they stopped)
# (a preparation run says where it is every 10 s: one silent this long is stuck (a run under Wine stopped once with all its
# threads waiting, 23/11/2026), is ended and goes on where it stopped, as exit 14)
PREP_SILENT_S = 900
SILENT_EXIT = 15


def crashed(code):
    """A run that ended on a crash: an NTSTATUS error on Windows (0xC0000005 an access violation, 0xC00000FD a stack overflow:
    a run under Wine ended so once, 24/11/2026, its cause not found), a signal on Linux"""
    return (code >= 0xC0000000) if os.name == "nt" else code < 0


def quiet_crashes():
    """(windows) No window when a program the builder starts crashes, the preparation or a compiler: the error mode the
    children inherit (SEM_NOGPFAULTERRORBOX: Windows' error reporting, and a debugger's dialog under Wine, are not started;
    the program ends with its code, which the builder reads). The preparation then goes on where it stopped"""
    if os.name != "nt":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        k.SetErrorMode(k.GetErrorMode() | 0x0001 | 0x0002 | 0x8000)   # (FAILCRITICALERRORS, NOGPFAULTERRORBOX, NOOPENFILEERRORBOX)
    except (OSError, AttributeError):
        pass
PREP_MIN_ENTRIES = 40000  # (a whole preparation makes ~59,000 cache entries: fewer is a preparation cut short)
PREP_PROGRESS = re.compile(r"vk: prepare: (\d+) s: step (\d) of 3, ([^,]+), item (\d+) of (\d+) \((\d+)% of the work, (.+?) left\); "
                           r"(\d+) pipelines built; (-?\d+) MiB free")


class BuildError(Exception):
    def __init__(self, code, message, details="", log=None):
        super().__init__(message)
        self.code, self.message, self.details, self.log = code, message, details, log


@dataclasses.dataclass
class Options:
    nup: str = ""                    # the player's game file
    out: str = ""                    # where homebrew/ and data/ go
    work: str = ""                   # the build's own files (default <out>/grapnel-work)
    toolchain: str = ""              # the grapnel-toolchain folder (default: the port's own PC's places)
    llvm: str = ""                   # LLVM 19.1.7's folder or bin folder (default: found)
    jobs: int = 0                    # compiles at once (0: the cores less one)
    lift_jobs: int = 0               # the translation's processes (0: by the memory free)
    memory: float = 0                # GB the compile may use (0: the memory free decides)
    python: str = sys.executable     # the Python for the translation and the build
    prompt_python: str = ""          # the Python for the button icons (default: python)
    no_prompts: bool = False         # no button icons
    title_art: str = ""              # a folder with icon0.png, pic0.dds, pic1.dds, hookshot_menu_fond.rgba (default: art/)
    lock: str = ""                   # a file to lock while translating and compiling (the port's PC)
    measure: bool = False            # sample the build's memory (Linux)
    no_shaders: bool = False         # no list of the game's shaders, so no preparation: the game compiles them in play
    no_prepare: bool = False         # the lists, but no preparation on this PC (a toolchain without prepare/): the game compiles in play
    prepare_workers: int = 0         # the preparation's compile threads (0: every core; the memory free decides how many work)


class Events:
    """What the core says; a front end overrides what it shows. This one says nothing"""

    def step(self, index, total, name):
        pass

    def progress(self, fraction, text=""):
        pass

    def log(self, line):
        pass

    def info(self, text):
        pass

    def step_done(self, index, seconds, peak_mib):
        pass


# ---- the measure: the builder's processes' memory (PSS, Linux /proc), sampled ----

class Sampler(threading.Thread):
    """The summed PSS of this process and every process under it, every `every` seconds: the peak since reset(). PSS shares a
    page between the processes mapping it, so forked lift workers are not counted once each for the pages they share"""

    def __init__(self, every=1.0):
        super().__init__(daemon=True)
        self.every, self.peak, self.done = every, 0, threading.Event()

    @staticmethod
    def available():
        return os.path.exists("/proc/self/smaps_rollup")

    @staticmethod
    def tree():
        kids = {}
        for p in os.listdir("/proc"):
            if p.isdigit():
                try:
                    with open("/proc/%s/stat" % p) as f:
                        ppid = int(f.read().rsplit(")", 1)[1].split()[1])
                    kids.setdefault(ppid, []).append(int(p))
                except (OSError, ValueError, IndexError):
                    pass
        out, todo = [], [os.getpid()]
        while todo:
            p = todo.pop()
            out.append(p)
            todo += kids.get(p, [])
        return out

    @staticmethod
    def pss_kb(pid):
        try:
            with open("/proc/%d/smaps_rollup" % pid) as f:
                for line in f:
                    if line.startswith("Pss:"):
                        return int(line.split()[1])
        except (OSError, ValueError):
            pass
        return 0

    def run(self):
        while not self.done.wait(self.every):
            self.peak = max(self.peak, sum(self.pss_kb(p) for p in self.tree()))

    def reset(self):
        peak, self.peak = self.peak, 0
        return peak


def mem_available_gb():
    if os.name == "nt":   # (grapnel, Windows: the available physical memory)
        return windows_memory()[1]
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 2 ** 20
    except (OSError, ValueError):
        pass
    return None


WINDOWS_ENV = ("USERPROFILE", "LOCALAPPDATA", "APPDATA", "PROGRAMDATA", "WINDIR", "COMSPEC", "PATHEXT", "NUMBER_OF_PROCESSORS",
               "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "OS")   # (grapnel, Windows: what its programs expect to find)


def windows_memory():
    """(grapnel, Windows) (total, available) physical memory in GiB, GlobalMemoryStatusEx; (None, None) when it does not answer"""
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32), ("total", ctypes.c_uint64), ("avail", ctypes.c_uint64),
                    ("page_total", ctypes.c_uint64), ("page_avail", ctypes.c_uint64), ("virtual_total", ctypes.c_uint64),
                    ("virtual_avail", ctypes.c_uint64), ("extended", ctypes.c_uint64)]
    st = Status()
    st.length = ctypes.sizeof(Status)
    try:
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return st.total / 2 ** 30, st.avail / 2 ** 30
    except (OSError, AttributeError):
        pass
    return None, None


def find_llvm_bin(given):
    """(grapnel) LLVM 19.1.7 from its bin folder alone, with no llvm-config (the Windows builder's own LLVM): clang and ld.lld say
    19.1.7, and llvm-ar, llvm-nm, llvm-objcopy are there. The same dict as find_llvm, builtins None (the toolchain folder has them)"""
    x = ".exe" if os.name == "nt" else ""
    seen = []
    for bindir in ([os.path.join(os.path.abspath(given), "bin"), os.path.abspath(given)] if given else []):
        missing = [n + x for n in ("clang", "ld.lld", "llvm-ar", "llvm-nm", "llvm-objcopy") if not os.path.isfile(os.path.join(bindir, n + x))]
        if missing:
            seen.append("%s: no %s" % (bindir, ", ".join(missing)))
            continue
        clang = os.path.join(bindir, "clang" + x)
        try:
            line = (subprocess.run([clang, "--version"], capture_output=True, text=True).stdout.splitlines() or [""])[0]
            lld = subprocess.run([os.path.join(bindir, "ld.lld" + x), "--version"], capture_output=True, text=True).stdout.strip()
        except OSError as e:
            seen.append("%s: %s" % (bindir, e))
            continue
        if LLVM_VERSION not in line or LLVM_VERSION not in lld:
            seen.append("%s: clang says %r, ld.lld %r" % (bindir, line, lld))
            continue
        return {"config": None, "bindir": bindir, "clang": clang, "version": line, "builtins": None}
    raise BuildError("llvm", "LLVM %s was not found (%s).\n%s" % (LLVM_VERSION, "; ".join(seen) or "no folder given",
                                                                   "The builder's own folder has it in llvm\\bin: unpack the "
                                                                   "whole zip again." if os.name == "nt" else LLVM_HELP))


def cpu_count():
    return len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 2)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def crc32(path):
    c = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            c = zlib.crc32(block, c)
    return c


def python_ok(py, modules):
    r = subprocess.run([py, "-I", "-c", "import %s" % ", ".join(modules)], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr.strip().splitlines() or [""])[-1]


class Lock:
    """An exclusive lock on a file while the heavy steps run (on the port's PC: build/heavy.lock); nothing without a path"""

    def __init__(self, path, events):
        self.path, self.events, self.fd = path, events, None

    def __enter__(self):
        if self.path:
            import fcntl
            self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o664)
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                self.events.info("waiting for %s (another heavy build holds it)" % self.path)
                fcntl.flock(self.fd, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        if self.fd is not None:
            import fcntl
            fcntl.flock(self.fd, fcntl.LOCK_UN)
            os.close(self.fd)
            self.fd = None


def describe():
    try:
        import version
        return version.describe(ROOT)
    except Exception as e:   # (the stamp is for the notes: never a reason to stop)
        return "unknown (%s)" % e


# ---- LLVM 19.1.7 ----

LLVM_HELP = """Grapnel is compiled with LLVM %(v)s only (clang, clang++, lld, llvm-ar, llvm-nm, llvm-objcopy and compiler-rt's
builtins), so that every player's eboot is the one the project tested. Ways to get it:
  - Debian 13 (trixie): sudo apt install clang-19 lld-19 llvm-19 libclang-rt-19-dev   (Debian's 19 is %(v)s)
  - Ubuntu and others: LLVM's own packages, https://apt.llvm.org (llvm.sh 19 installs the last 19.x, %(v)s), or the release
    archive for Linux x86-64 from https://github.com/llvm/llvm-project/releases/tag/llvmorg-%(v)s, unpacked anywhere
then build again, with --llvm <that LLVM's folder, or its bin folder> if it is not found by itself.""" % {"v": LLVM_VERSION}


def llvm_candidates(given):
    if given:
        g = os.path.abspath(given)
        return [p for p in (os.path.join(g, "bin", "llvm-config"), os.path.join(g, "llvm-config"), g) if os.path.isfile(p)] or [g]
    out = []
    if os.environ.get("GRAPNEL_LLVM"):
        out += llvm_candidates(os.environ["GRAPNEL_LLVM"])
    for name in ("llvm-config-19", "llvm-config"):
        p = shutil.which(name)
        if p:
            out.append(p)
    out.append("/usr/lib/llvm-19/bin/llvm-config")
    return out


def find_llvm(given, need_builtins=True):
    """{config, bindir, clang, version line, builtins} for LLVM 19.1.7, or BuildError("llvm") with what was found and how to get it.
    need_builtins False: a toolchain folder with its own compiler-rt builtins (0.2, llvm-rt/) makes them unneeded here"""
    if os.name == "nt" or (given and not any(os.path.isfile(c) and os.path.basename(c).startswith("llvm-config")
                                             for c in llvm_candidates(given))):
        return find_llvm_bin(given)
    seen = []
    for config in llvm_candidates(given):
        if not os.path.isfile(config):
            seen.append("%s: not there" % config)
            continue
        r = subprocess.run([config, "--version"], capture_output=True, text=True)
        version = r.stdout.strip()
        if r.returncode or not version.startswith(LLVM_VERSION):
            seen.append("%s: version %s" % (config, version or "?"))
            continue
        bindir = subprocess.run([config, "--bindir"], capture_output=True, text=True).stdout.strip()
        missing = [n for n in ("clang", "clang++", "ld.lld", "llvm-ar", "llvm-nm", "llvm-objcopy") if not os.path.isfile(os.path.join(bindir, n))]
        if missing:
            seen.append("%s: %s has no %s" % (config, bindir, ", ".join(missing)))
            continue
        clang = os.path.join(bindir, "clang")
        line = subprocess.run([clang, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        lld = subprocess.run([os.path.join(bindir, "ld.lld"), "--version"], capture_output=True, text=True).stdout.strip()
        if LLVM_VERSION not in line or LLVM_VERSION not in lld:
            seen.append("%s: clang says %r, ld.lld %r" % (config, line, lld))
            continue
        res = subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True).stdout.strip()
        builtins = os.path.join(res, "lib", "linux", "libclang_rt.builtins-x86_64.a")
        if need_builtins and not os.path.isfile(builtins):
            other = os.path.join(res, "lib", "x86_64-unknown-linux-gnu", "libclang_rt.builtins.a")
            hint = (" (this LLVM has it as %s: make the name PS5_Vulkan's link recipe reads with: mkdir -p %s && ln -s %s %s)"
                    % (other, os.path.dirname(builtins), other, builtins)) if os.path.isfile(other) else \
                   " (Debian: the package libclang-rt-19-dev)"
            seen.append("%s: no compiler-rt builtins %s%s" % (config, builtins, hint))
            continue
        return {"config": config, "bindir": bindir, "clang": clang, "version": line, "builtins": builtins}
    raise BuildError("llvm", "LLVM %s was not found (%s).\n%s" % (LLVM_VERSION, "; ".join(seen), LLVM_HELP))


# ---- the console toolchain ----

def tools(toolchain):
    """Where the console's tools are: the toolchain folder (make_toolchain.py's layout), else the port's PC's places"""
    if toolchain:
        t = toolchain
        return {"sdk": os.path.join(t, "sdk"), "ps5_vulkan": os.path.join(t, "ps5_vulkan"),
                "radv": os.path.join(t, "radv", "lib", "libvulkan_radeon.ps5.a"), "cpm": os.path.join(t, "cpm"),
                "dav1d": os.path.join(t, "dav1d"), "dav1d_include": os.path.join(t, "dav1d", "include-src"), "bundle": t,
                "recipe": os.path.join(t, "ps5_recipe.json") if os.path.isfile(os.path.join(t, "ps5_recipe.json")) else None,
                "prepare": os.path.join(t, "prepare")}
    above = toolchain_ps5._above
    return {"sdk": os.environ.get("PS5_PAYLOAD_SDK") or above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk"),
            "ps5_vulkan": os.environ.get("PS5_VULKAN") or above("ps5-stack", "PS5_Vulkan"),
            "radv": os.environ.get("RADV_ARCHIVE") or above("mesa-work", "game-build", "radv-release-sync", "lib", "libvulkan_radeon.ps5.a"),
            "cpm": os.environ.get("RX_CPM") or above("wt", "ps5-port", "third_party-ps5", "cpm"),
            "dav1d": os.environ.get("HOOKSHOT_PS5_DAV1D") or above("build", "dav1d-ps5"),
            "dav1d_include": above("third_party", "dav1d", "include"), "bundle": None,
            "prepare": os.environ.get("GRAPNEL_PREPARE")}


def check_bundle(t):
    """The toolchain folder's files against its toolchain.json (sizes and SHA-256): a missing or changed file is said"""
    path = os.path.join(t["bundle"], "toolchain.json")
    if not os.path.isfile(path):
        raise BuildError("toolchain", "%s is not a Grapnel toolchain folder (no toolchain.json)" % t["bundle"])
    info = json.load(open(path))
    bad = []
    for rel, want in info["files"].items():
        p = os.path.join(t["bundle"], *rel.split("/"))
        if not os.path.isfile(p) or os.path.getsize(p) != want["bytes"] or sha256(p) != want["sha256"]:
            bad.append(rel)
    if bad:
        raise BuildError("toolchain", "the toolchain folder %s is not as its toolchain.json says (%d files missing or changed: %s): "
                         "unpack it again" % (t["bundle"], len(bad), ", ".join(bad[:5])))
    return info


def tools_env(t, llvm, work=None):
    """The tools' environment: PATH with LLVM 19.1.7 first, the SDK's wrappers pinned to it (LLVM_CONFIG: they take the newest
    llvm-config-NN on PATH otherwise), the console's tools; nothing of a developer's RX_/HOS_/LONGSHOT_ switches"""
    env = {k: os.environ[k] for k in ("HOME", "USER", "TMPDIR", "SYSTEMROOT", "TEMP", "TMP") + WINDOWS_ENV if k in os.environ}
    env.update(LANG="C.UTF-8", PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8",
               PATH=llvm["bindir"] + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"), LLVM_CONFIG=llvm["config"] or "",
               PS5_CLANG=llvm["clang"],
               EMSDK=os.path.join(work or ROOT, "no-emsdk"),   # (a64-kit's toolchain.find looks in ~/emsdk first: never here)
               RX_TARGET="ps5")
    if t.get("recipe"):   # (0.2: no bash, the LLVM tools run with the recipe's arguments: nx2/toolchain_ps5.py direct mode)
        env.update(PS5_RECIPE=t["recipe"], PS5_LLVM_BIN=llvm["bindir"])
    for key, var in (("sdk", "PS5_PAYLOAD_SDK"), ("ps5_vulkan", "PS5_VULKAN"), ("radv", "RADV_ARCHIVE"), ("cpm", "RX_CPM"),
                     ("dav1d", "HOOKSHOT_PS5_DAV1D"), ("dav1d_include", "HOOKSHOT_PS5_DAV1D_INCLUDE")):
        if t.get(key):
            env[var] = t[key]
    return env


# ---- the shaders' preparation on the PC (step 7) ----

def checkout_tree(rel):
    """The git tree of a folder of this checkout, "" when it cannot be said: a player's app folder is a git archive, and a
    player's PC may have no git at all (Windows' rarely has: running it raised FileNotFoundError and stopped the build)"""
    try:
        r = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD:" + rel], capture_output=True, text=True)
    except OSError:
        return ""
    return r.stdout.strip() if r.returncode == 0 else ""


# (the host and the renderer, copied into the run's folder beside the files they read; Windows' driver stays in the tool's folder)
PREP_PROGRAMS = ("longshot.exe", "longshot_vk.dll") if os.name == "nt" else ("longshot", "longshot_vk.so")


def prepare_tool(t):
    """The toolchain's shader preparation (prepare/, make_prepare_tool.py): its files as its prepare.json says, this checkout's
    translator (the same shader stamp: else its cache is of no use to this eboot) and the console's driver (the same build ID)"""
    d = t.get("prepare")
    path = os.path.join(d, "prepare.json") if d else ""
    if not d or not os.path.isfile(path):
        raise BuildError("prepare-tool", "the toolchain has no shader preparation for the PC (its prepare/ folder): without it the console "
                         "compiles the game's shaders as you play. Unpack this release's toolchain, or build with --no-prepare")
    info = json.load(open(path))
    # (the tool is made for one system: linux-x64, or windows-x64 since 0.3, make_prepare_tool.py --system)
    here = "windows-x64" if os.name == "nt" else "linux-x64" if sys.platform.startswith("linux") else sys.platform
    if info.get("system", "linux-x64") != here:
        raise BuildError("prepare-platform", "the toolchain's shader preparation runs on %s, not on this PC (%s): build with --no-prepare "
                         "(the console then compiles the shaders as you play), or take the toolchain made for this system"
                         % (info.get("system", "linux-x64"), here))
    bad = []
    for rel, want in info.get("files", {}).items():
        p = os.path.join(d, *rel.split("/"))
        if not os.path.isfile(p) or os.path.getsize(p) != want["bytes"] or sha256(p) != want["sha256"]:
            bad.append(rel)
    if bad:
        raise BuildError("prepare-tool", "the shader preparation %s is not as its prepare.json says (%d files missing or changed: %s): "
                         "unpack the toolchain again" % (d, len(bad), ", ".join(bad[:5])))
    import translator_files
    stamp = translator_files.stamp(os.path.join(ROOT, "gpu", "vk"))
    if info.get("shader_stamp") != stamp:
        raise BuildError("prepare-stale", "the toolchain's shader preparation translates the shaders as the renderer with the stamp %s, "
                         "this checkout's is %s: the cache it would make is of no use to this eboot. Take this release's toolchain"
                         % (info.get("shader_stamp"), stamp))
    if info.get("driver_build_id") != CONSOLE_DRIVER_BUILD_ID:
        raise BuildError("prepare-stale", "the toolchain's shader preparation models the driver %s, not the console's %s"
                         % (info.get("driver_build_id"), CONSOLE_DRIVER_BUILD_ID))
    return d, info


def prepare_env(tool, run, cache, workers):
    """The preparation's environment: the public settings the console runs with (data/hookshot_env.txt) less its platform's, the
    console build's defaults a PC build lacks, the toolchain's driver and libraries, nothing of the player's own Vulkan layers
    or of a developer's switches"""
    tool, run, cache = os.path.abspath(tool), os.path.abspath(run), os.path.abspath(cache)   # (the run's folder is its own)
    env = {k: os.environ[k] for k in ("HOME", "USER", "TMPDIR", "SYSTEMROOT", "TEMP", "TMP") + WINDOWS_ENV if k in os.environ}
    if os.name == "nt":   # (windows: the system's own folders only; the tool's DLLs are beside its exe)
        root = os.environ.get("SYSTEMROOT", r"C:\Windows")
        env.update(PATH=os.pathsep.join((os.path.join(root, "System32"), root)))
    else:
        env.update(LANG="C.UTF-8", PATH="/usr/bin:/bin")
    with open(os.path.join(DATA, "hookshot_env.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.startswith("HOOKSHOT_PS5_") or k in PREP_PLATFORM_ONLY:
                continue
            env[k] = v
    env.update(PREP_CONSOLE_DEFAULTS)
    if os.name == "nt":   # (windows: the driver opened directly, no loader: gpu/vk/vk_main.cpp GRAPNEL_VK_ICD)
        env["GRAPNEL_VK_ICD"] = os.path.join(tool, "vulkan_radeon.dll")
    else:
        env["LD_LIBRARY_PATH"] = os.path.join(tool, "lib")
    env.update({
        "VK_DRIVER_FILES": os.path.join(tool, "icd.json"),
        "VK_ICD_FILENAMES": os.path.join(tool, "icd.json"), "VK_LOADER_LAYERS_DISABLE": "~implicit~",
        "XDG_CACHE_HOME": os.path.join(run, "xdg-cache"),
        "MESA_SHADER_CACHE_DIR": cache, "MESA_DISK_CACHE_DATABASE": "1", "MESA_DISK_CACHE_DATABASE_EXCLUSIVE": "1",
        "HOS_HEADLESS": "1", "HOS_DISCORD": "0", "HOS_CRASH_WATCH": "0", "HOOKSHOT_NO_UPDATE_CHECK": "1",
        "LONGSHOT_NO_UPDATE_CHECK": "1", "RX_VK_SKIP_FILE": os.path.join(run, "skip.txt"),
        "RX_VK_CONSOLE_PREPARE_RUN": "1", "HOOKSHOT_PS5_PREPARE_WORKERS": str(workers)})
    if os.name == "nt":
        # (windows: the folders the driver and the renderer open by their narrow names, relative to the run's folder, which is
        # their working folder: plain ASCII whatever the player's name. Under Wine an absolute "C:\users\Prénom Nom\..." came to
        # the driver one byte short, "...\radv-shader-cach", and the cache went beside the one the builder collects)
        try:
            env["MESA_SHADER_CACHE_DIR"] = os.path.relpath(cache, run)
            env["RX_VK_SKIP_FILE"] = os.path.relpath(env["RX_VK_SKIP_FILE"], run)
        except ValueError:   # (another drive: as they are)
            pass
    return env


# (the preparation's three steps and their share of its time on a PC, measured: a whole run under Wine on 14 threads, 187 s,
# 342 s and 827 s; the renderer's own "% of the work" counts in its units, which made the start's estimate far too long: 1 h 18
# left at 313 s of a 1,359 s run)
PREP_START = re.compile(r"\((\d+) mesh,.*?; (\d+) compute\), \d+ pipeline records in (\d+) programs.*?from item (\d+)")
PREP_STEP_SHARE = (0.138, 0.252, 0.610)


class PrepEstimate:
    """The time left of a preparation run from its own pace step by step: the first step's items come at an even rate, so its
    length is known early and the whole run is that length over its share; the second step's items end in no useful order,
    so the estimate made at its start holds until it is over; in the third step the renderer's own figure is good (its
    items end faster than they begin), and the rest at the step's item rate bounds it. A run that starts past the first item
    (a run again) is left to the renderer's own figures"""

    def __init__(self):
        self.counts = None
        self.starts = {}

    def start_line(self, line):
        m = PREP_START.search(line)
        if m:
            n1, n2, n3, first = (int(x) for x in m.groups())
            self.counts = (n1, n2, n3) if first == 0 else None

    def at(self, seconds, step, item, theirs=None):
        """(fraction done, seconds left) for a progress line (theirs: the renderer's seconds left), or None when there is
        nothing to go on"""
        if not self.counts or step not in (1, 2, 3) or seconds <= 0:
            return None
        # (a step began after the previous step's last line: lines come every 10 s, so at most that much later)
        self.starts.setdefault(step, getattr(self, "seen", seconds))
        self.seen = seconds
        n1, n2, n3 = self.counts
        if step == 1:
            g = min(1.0, item / max(1, n1))
            if g < 0.05:
                return None
            total = seconds / g / PREP_STEP_SHARE[0]
        elif step == 2:
            total = self.starts[2] / PREP_STEP_SHARE[0]
        else:
            g = min(1.0, max(0.0, (item - n1 - n2) / max(1, n3)))
            begun = self.starts[3]
            if g < 0.05:
                total = begun / (PREP_STEP_SHARE[0] + PREP_STEP_SHARE[1])
            else:
                total = begun + (seconds - begun) / g
            if theirs is not None:
                total = min(total, seconds + theirs)
        total = max(total, seconds + 1)
        return seconds / total, total - seconds


def read_left(text):
    """The renderer's time left ("< 1 min", "N min", "H h MM") in seconds; None if it says otherwise"""
    m = re.fullmatch(r"\s*(?:(<) 1 min|(\d+) min|(\d+) h (\d+))\s*", text or "")
    if not m:
        return None
    return 30 if m.group(1) else int(m.group(2)) * 60 if m.group(2) else (int(m.group(3)) * 60 + int(m.group(4))) * 60


def say_left(seconds):
    """The renderer's way of saying a time left: "< 1 min", "N min", "H h MM" """
    m = int(seconds // 60)
    return "< 1 min" if m < 1 else "%d min" % m if m < 60 else "%d h %02d" % (m // 60, m % 60)


def mesa_db_facts(cache):
    """The Mesa cache database below cache (mesa_shader_cache_db/part<N>/mesa_cache.db and .idx, src/util/mesa_cache_db.c): each
    part read entry by entry (32-byte key, CRC, size, blob) and its index's count; a part whose two disagree, or cut short, is
    said. {"parts", "entries", "bytes", "bad"}"""
    root = os.path.join(cache, "mesa_shader_cache_db")
    facts = {"parts": 0, "entries": 0, "bytes": 0, "bad": []}
    if not os.path.isdir(root):
        return facts
    for part in sorted(os.listdir(root)):
        db, idx = os.path.join(root, part, "mesa_cache.db"), os.path.join(root, part, "mesa_cache.idx")
        if not os.path.isfile(db):
            continue
        n, size = 0, os.path.getsize(db)
        with open(db, "rb") as f:
            head = f.read(20)
            ok = head[:7] == b"MESA_DB"
            at = 20
            while ok and at + 40 <= size:
                f.seek(at + 36)
                blob, = struct.unpack("<I", f.read(4))
                if at + 40 + blob > size:
                    ok = False
                    break
                n, at = n + 1, at + 40 + blob
        indexed = (os.path.getsize(idx) - 20) // 28 if os.path.isfile(idx) else -1
        if not ok or indexed != n:
            facts["bad"].append("%s (%d entries, %d indexed%s)" % (part, n, indexed, "" if ok else ", cut short"))
        facts["parts"] += 1
        facts["entries"] += n
        facts["bytes"] += size + max(0, os.path.getsize(idx) if os.path.isfile(idx) else 0)
    return facts


def mesa_db_digest(cache):
    """What the console's driver will find in the cache: each key once (its last entry, as the index has it) with its blob,
    in key order, hashed; the parts the entries are in left out (the driver looks in every part, and which part takes an
    entry depends on the workers' order). (keys, the digest's first 16 hex digits): the same on every PC that prepared the
    same game with the same tool, Windows or Linux"""
    root = os.path.join(cache, "mesa_shader_cache_db")
    seen = {}
    if os.path.isdir(root):
        for part in sorted(os.listdir(root)):
            db = os.path.join(root, part, "mesa_cache.db")
            if not os.path.isfile(db):
                continue
            with open(db, "rb") as f:
                d = f.read()
            at = 20
            while at + 40 <= len(d):
                size, = struct.unpack_from("<I", d, at + 36)
                seen[d[at:at + 32]] = hashlib.sha256(d[at + 40:at + 40 + size]).digest()
                at += 40 + size
    h = hashlib.sha256()
    for k in sorted(seen):
        h.update(k + seen[k])
    return len(seen), h.hexdigest()[:16]


def read_progress(path):
    """The preparation's progress file (gpu/vk/console_prepare.h format()): its lines as a dict, the lists as lists"""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            k, _, v = line.strip().partition(" ")
            if k in ("inflight", "strikes", "skip"):
                out[k] = [int(x) for x in v.split()]
            elif k:
                out[k] = v
    return out


# ---- what the checkout says about the title ----

def supported_build():
    """make_game.py's BUILD_ID: the one build of the game the patches, hot lists and profiles were made from"""
    with open(os.path.join(NX2, "make_game.py"), encoding="utf-8") as f:
        m = re.search(r'^BUILD_ID = "([0-9a-f]+)"', f.read(), re.M)
    if not m:
        raise BuildError("checkout", "nx2/make_game.py has no BUILD_ID")
    return m.group(1)


def data_folder_name():
    """The console's data folder (/data/<name>): PS5_DATA_NAME in host/posix/ps5_data_folder.h"""
    with open(os.path.join(ROOT, "host", "posix", "ps5_data_folder.h"), encoding="utf-8", errors="replace") as f:
        m = re.search(r'#define PS5_DATA_NAME "([A-Za-z0-9_.-]+)"', f.read())
    if not m:
        raise BuildError("checkout", "host/posix/ps5_data_folder.h names no PS5_DATA_NAME")
    return m.group(1)


def name_prefix():
    """The console's file names' prefix (<prefix>_env.txt...): HOS_NAME_PREFIX for __PROSPERO__ in host/hos_paths.h"""
    with open(os.path.join(ROOT, "host", "hos_paths.h"), encoding="utf-8", errors="replace") as f:
        m = re.search(r'#ifdef __PROSPERO__\s*#define HOS_NAME_PREFIX "([^"]+)"', f.read())
    return m.group(1) if m else "hookshot"


def check_nup(path):
    """The .nup is a NUP package whose newest title is the supported game's update: (title id, version), or BuildError"""
    if os.path.isdir(path):
        raise BuildError("nup-folder", "%s is a folder. Grapnel %s is built from the game's .nup file only (the console dump the "
                         "port reads as game.nup), not from unpacked exefs/ or romfs/ folders." % (path, VERSION))
    if not os.path.isfile(path):
        raise BuildError("nup-missing", "the game file %s was not found" % path)
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic != b"NUP\x01":
        raise BuildError("nup-not-nup", "%s is not a .nup package (it starts %r). Grapnel %s is built from the game's .nup only."
                         % (path, magic, VERSION))
    import nup
    try:
        p = nup.Nup(path)
    except SystemExit as e:   # (nup.py says an incomplete package this way)
        raise BuildError("nup-damaged", "%s: %s" % (path, e))
    try:
        t = nup.pick_title(p, None)
        found = ", ".join("%016X v%d" % (x["id"], x["version"]) for x in p.titles)
    finally:
        p.f.close()
    if (t["id"], t["version"]) != (GAME_TITLE_ID, GAME_VERSION):
        raise BuildError("nup-wrong-game", "this .nup holds %s, its newest title %016X v%d. Grapnel is made from %s (title %016X v%d) "
                         "and no other game or version." % (found, t["id"], t["version"], GAME_NAME, GAME_TITLE_ID, GAME_VERSION))
    return t["id"], t["version"]


EXTRACT_PROMPTS = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
import nup as N
p = N.Nup(sys.argv[2])
t = N.pick_title(p, None)
prefixes = tuple(sys.argv[4:])
n = total = 0
for rel, r in p.walk(t["romfs"]):
    if rel.startswith(prefixes):
        dst = os.path.join(sys.argv[3], *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst + ".part", "wb") as g:
            p.copy_to(r, g, rel)
        os.replace(dst + ".part", dst)
        n += 1
        total += r[2]
print("the button icons' game files read out of the .nup: %d files, %.1f MB (title v%d)" % (n, total / 1e6, t["version"]))
"""


EXTRACT_CENSUS = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
import nup as N
p = N.Nup(sys.argv[2])
t = N.pick_title(p, None)
folders, dictionary = tuple(sys.argv[4].split(",")), sys.argv[5]
n = total = 0
for rel, r in p.walk(t["romfs"]):
    name = rel.rsplit("/", 1)[-1]
    # (the files nx2/shader_census.py opens, by its own rule (archives()), and the dictionaries: no movie, no model, no texture)
    if not (rel == dictionary or (rel.startswith(folders) and (name.endswith(".zs") or ".bfsha" in name or ".sarc" in name or
                                                                ".pck" in name))):
        continue
    dst = os.path.join(sys.argv[3], *rel.split("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".part", "wb") as g:
        p.copy_to(r, g, rel)
    os.replace(dst + ".part", dst)
    n += 1
    total += r[2]
print("the shaders' game files read out of the .nup: %d files, %.1f MB (title v%d)" % (n, total / 1e6, t["version"]))
"""


def census_facts(path):
    """What the renderer reads of a census (gpu/vk/vk_shader.cpp seed_open; vk_draw.cpp census_kinds): its head, every stage's
    head and size, every program's stages, each bound checked as seed_open checks it. Programs that are compute (a stage 5), mesh
    (a stage 7) and mesh with a task stage (6). A BuildError when the file is not one the renderer takes"""
    import struct
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(16)
        if len(head) < 16 or head[:8] != b"HKSEED01":
            raise BuildError("census-wrong", "%s is not a census of the game's shaders (its head)" % path)
        nprog, nstage = struct.unpack_from("<II", head, 8)
        if nstage > (size - 16) // 20 or nprog > (size - 16) // 4:
            raise BuildError("census-wrong", "%s: counts past its size" % path)
        at, kinds = 16, []
        for _ in range(nstage):
            h = f.read(20)
            if len(h) < 20:
                raise BuildError("census-wrong", "%s is cut short in its stages" % path)
            stage, csize, nbytes = struct.unpack_from("<III", h, 8)
            if csize > size - at - 20 or nbytes > size - at - 20 - csize:
                raise BuildError("census-wrong", "%s: a stage past its end" % path)
            kinds.append(stage & 7)
            at += 20 + csize + nbytes
            f.seek(at)
        compute = mesh = task = 0
        for _ in range(nprog):
            h = f.read(4)
            if len(h) < 4:
                raise BuildError("census-wrong", "%s is cut short in its programs" % path)
            n, = struct.unpack("<I", h)
            if n > (size - at - 4) // 4:
                raise BuildError("census-wrong", "%s: a program past its end" % path)
            ids = struct.unpack("<%dI" % n, f.read(4 * n))
            if any(i >= nstage for i in ids):
                raise BuildError("census-wrong", "%s: a program names a stage it does not have" % path)
            k = {kinds[i] for i in ids}
            compute += 5 in k
            mesh += 5 not in k and 7 in k
            task += 5 not in k and 7 in k and 6 in k
            at += 4 + 4 * n
    return {"programs": nprog, "stages": nstage, "compute": compute, "mesh": mesh, "mesh_task": task, "bytes": size}


def make_state_list(dst):
    """The pipelines the console's preparation builds: assets/pipeline_states/ merged as nx2/make_game.py merges it for the PC,
    version 2 (nx2/pipeline_states.py), less STATES_LEFT_OUT; written to dst. (records, the records left out)"""
    import pipeline_states
    folder = pipeline_states.sources_dir(ROOT)
    kept, dropped = [], 0
    for path in pipeline_states.sources(folder):
        recs = pipeline_states.read(path)
        if os.path.basename(path).startswith(STATES_LEFT_OUT):
            dropped += len(recs)
            continue
        kept.append(recs)
    lists, lost = pipeline_states.as_v2(kept, force=True)
    recs = pipeline_states.unique(lists)
    with open(dst + ".part", "wb") as f:
        f.write(pipeline_states.pack(recs))
    os.replace(dst + ".part", dst)
    return len(recs), dropped + lost


def tree_sha1(folder):
    h = hashlib.sha1()
    for name in sorted(os.listdir(folder)):
        p = os.path.join(folder, name)
        if os.path.isfile(p):
            h.update(name.encode() + b"\0")
            with open(p, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
    return h.hexdigest()


def llvm_stamp(llvm):
    """(grapnel) Which LLVM compiles: clang's version line and its bin folder's files by name and size. Two builds of one LLVM
    version can compile differently (llvm-mingw's, built on libc++, made another vk_texture.o than every libstdc++-built clang:
    notes-agents/constructeur-windows.md), and nx2/build.py's object keys name the flags and sources, not the compiler"""
    b = llvm["bindir"]
    files = sorted("%s %d" % (n, os.path.getsize(os.path.join(b, n))) for n in os.listdir(b) if os.path.isfile(os.path.join(b, n)))
    return "%s\n%s\n" % (llvm["version"], "\n".join(files))


def objects_from_another_llvm(work, mark, now):
    """A work folder whose objects another LLVM compiled, or a builder before the mark (which cannot say): its objects go"""
    if not os.path.isdir(os.path.join(work, "sa", "build-ps5")):
        return False
    try:
        with open(mark, encoding="utf-8") as f:
            return f.read() != now
    except OSError:
        return True


class CompileProgress:
    """The compile's progress from nx2/build.py's lines, by each file's share of the compile time (make_game.py's measure: the hot
    files 23.5 s per MB of C, the other lifted files 3.4 s per MB + 1, the rest ~1.3 s each), not by the count of files"""

    def __init__(self, gen):
        self.gen, self.done, self.total = gen, 0.0, 0.0

    @staticmethod
    def cost(name, mb):
        return 23.5 * mb if name.startswith("code_hot") else 3.4 * mb + 1 if name.startswith("code_") else 1.3

    def line(self, line):
        """the fraction done after this line, or None when the line says nothing of it"""
        m = re.match(r"(\d+) files, ", line)
        if m:
            ours = [f for f in os.listdir(self.gen) if f.startswith("code_") and f.endswith(".c")] if os.path.isdir(self.gen) else []
            self.total = sum(self.cost(f, os.path.getsize(os.path.join(self.gen, f)) / (1 << 20)) for f in ours) + \
                max(0, int(m.group(1)) - len(ours)) * 1.3
            return 0.0
        m = re.match(r"\s+(\S+)\s+(\d+) MB\s+\d+s", line)
        if m and self.total:
            self.done += self.cost(m.group(1), float(m.group(2)))
            return min(0.99, self.done / self.total)
        return None


class Builder:
    def __init__(self, options, events=None):
        self.o = options
        self.ev = events or Events()
        self.o.out = os.path.abspath(self.o.out) if self.o.out else ""
        self.o.work = os.path.abspath(self.o.work or (os.path.join(self.o.out, "grapnel-work") if self.o.out else ""))
        self.o.prompt_python = self.o.prompt_python or self.o.python
        self.o.jobs = self.o.jobs or max(1, cpu_count() - 1)
        self.logs = os.path.join(self.o.work, "logs") if self.o.work else None
        self.sampler = Sampler() if self.o.measure and Sampler.available() else None
        if self.sampler:
            self.sampler.start()
        self.times = []   # (step name, seconds, peak PSS in MiB or None)
        self.llvm = self.t = self.env = None
        self.tool_notes = {}
        self.exefs = self.romfs = self.bid = None
        self.seen = {}
        self.title = self.data = self.data_name = None
        self.art = []
        self.icons_dir = None
        self.shader_files = None
        self.prep_tool = None
        self.prepared = None

    # ---- running the port's tools ----

    def _run(self, cmd, log, what, on_line=None, env=None, cwd=None, idle=None):
        """cmd's output to the events and to log as it comes; a failure is a BuildError with its last lines. idle: seconds
        without a line after which cmd is ended, its exit then SILENT_EXIT"""
        os.makedirs(os.path.dirname(log), exist_ok=True)
        tail = []
        with open(log, "a", encoding="utf-8") as f:
            f.write("$ %s\n" % " ".join(cmd))
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                 env=env or self.env, cwd=cwd or ROOT)
            heard, silent = [time.time()], []
            if idle:
                def watch():
                    while p.poll() is None:
                        time.sleep(5)
                        if time.time() - heard[0] > idle:
                            silent.append(True)
                            p.kill()   # (this run's own process; on Windows the build's job holds it too)
                            return
                threading.Thread(target=watch, daemon=True).start()
            for line in p.stdout:
                heard[0] = time.time()
                line = line.rstrip("\n")
                f.write(line + "\n")
                tail = (tail + [line])[-30:]
                self.ev.log(line)
                if on_line:
                    on_line(line)
            rc = p.wait()
            if silent:
                f.write("silent for %d s: ended\n" % idle)
                rc = SILENT_EXIT
            f.write("exit %d\n" % rc)
        if rc:
            raise BuildError("tool-failed", "%s failed (exit %d)" % (what, rc), "\n".join(tail[-12:]), log)

    def _step(self, n, fn):
        if self.sampler:
            self.sampler.reset()
        self.ev.step(n, len(STEPS), STEPS[n - 1])
        t0 = time.time()
        r = fn()
        peak = self.sampler.reset() / 1024 if self.sampler else None
        self.times.append((STEPS[n - 1], time.time() - t0, peak))
        self.ev.step_done(n, time.time() - t0, peak)
        return r

    # ---- the check: everything but the game ----

    def check(self):
        """LLVM, the console toolchain, Python's packages, ps5-native-tool, the ZBIC program, a compile for the console"""
        self.ev.info("Grapnel builder %s, checkout %s" % (VERSION, describe()))
        self.t = t = tools(self.o.toolchain)
        self.llvm = find_llvm(self.o.llvm, need_builtins=not t.get("recipe"))
        self.ev.info("ok: LLVM %s (%s)" % (self.llvm["version"], self.llvm["bindir"]))
        self.tool_notes = {"llvm": self.llvm["version"], "toolchain": None}
        if t["bundle"]:
            info = check_bundle(t)
            self.tool_notes["toolchain"] = "%s %s (%s)" % (info["name"], info["version"], info["made"])
            self.ev.info("ok: toolchain %s, %d files as its toolchain.json says" % (self.tool_notes["toolchain"], len(info["files"])))
        else:
            self.ev.info("note: no toolchain folder given: the console tools are taken where the port's own PC has them")
        missing = [k for k in ("sdk", "ps5_vulkan", "radv", "cpm", "dav1d", "dav1d_include") if not t.get(k) or not os.path.exists(t[k])]
        if missing:
            raise BuildError("toolchain", "the console toolchain is missing %s: give the unpacked grapnel-toolchain folder" % ", ".join(missing))
        for f in ((t["recipe"],) if t.get("recipe") else (os.path.join(t["sdk"], "bin", "prospero-clang"),
                                                          os.path.join(t["ps5_vulkan"], "tools", "radv-link.sh"))) + \
                 (os.path.join(t["ps5_vulkan"], "runtime", "libc.prx"), os.path.join(t["dav1d"], "src", "libdav1d.a")) + \
                tuple(os.path.join(t["cpm"], d) for d in ("vulkanheaders", "spirv-headers", "vulkanmemoryallocator")):
            if not os.path.exists(f):
                raise BuildError("toolchain", "the console toolchain has no %s" % f)
        self.env = tools_env(t, self.llvm, self.o.work or None)
        self.prep_tool = None
        if not (self.o.no_shaders or self.o.no_prepare):
            self.prep_tool, info = prepare_tool(t)
            self.tool_notes["prepare"] = "%s %s (%s), shader stamp %s, driver %s" % (info["name"], info["version"], info["made"],
                                                                                    info["shader_stamp"], info["driver_build_id"][:12])
            self.ev.info("ok: the shader preparation for the PC, %s" % self.tool_notes["prepare"])
            tree = checkout_tree("gpu/vk")
            if info.get("gpu_vk_tree") and tree and tree != info["gpu_vk_tree"]:
                self.ev.info("note: the preparation's renderer is of another revision than this checkout's (the same translator): "
                             "a pipeline this checkout builds otherwise is compiled on the console when first drawn")
        self._native_tool()
        ok, err = python_ok(self.o.python, LIFT_MODULES)
        if not ok:
            raise BuildError("python", "the Python %s lacks the translator's packages (%s): pip install -r %s"
                             % (self.o.python, err, os.path.join(HERE, "requirements.txt")))
        self.ev.info("ok: Python %s: %s" % (self.o.python, " ".join(LIFT_MODULES)))
        if not self.o.no_prompts:
            ok, err = python_ok(self.o.prompt_python, PROMPT_MODULES)
            if not ok:
                raise BuildError("python-icons", "the Python %s lacks the button icons' packages (%s): pip install -r %s (or no icons: "
                                 "the game then shows the Switch's)" % (self.o.prompt_python, err, os.path.join(HERE, "requirements.txt")))
            self.ev.info("ok: Python %s: %s" % (self.o.prompt_python, " ".join(PROMPT_MODULES)))
        r = subprocess.run([self.o.python, "-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); import zbic; print(zbic.selftest())",
                            NX2], capture_output=True, text=True, env=self.env)
        if r.returncode:
            raise BuildError("zbic", "the ZBIC program (nx2/zbic_unpack) did not build or does not unpack its own frames",
                             (r.stdout + r.stderr)[-1500:])
        self.ev.info("ok: the ZBIC program: %s" % r.stdout.strip().splitlines()[-1])
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "check.c")
            with open(c, "w") as f:
                f.write("#include <stdint.h>\n#include <string.h>\nuint64_t check(const char *s) { return strlen(s) + 1; }\n")
            args = ["-march=x86-64-v3", "-O1", "-c", c, "-o", os.path.join(tmp, "check.o"), "-v"]
            if t.get("recipe"):   # (the build's own way: nx2/toolchain_ps5.py's direct mode reads these two from the environment)
                os.environ.update(PS5_RECIPE=self.env["PS5_RECIPE"], PS5_LLVM_BIN=self.env["PS5_LLVM_BIN"])
                cmd = toolchain_ps5.command(toolchain_ps5.find("clang"), args)
            else:
                cmd = [os.path.join(t["sdk"], "bin", "prospero-clang")] + args
            r = subprocess.run(cmd, capture_output=True, text=True, env=self.env)
            if r.returncode:
                raise BuildError("toolchain", "prospero-clang does not compile for the console", (r.stdout + r.stderr)[-1500:])
            if LLVM_VERSION not in r.stderr:
                raise BuildError("llvm", "prospero-clang runs another clang than LLVM %s" % LLVM_VERSION, "\n".join(r.stderr.splitlines()[:3]))
        self.ev.info("ok: %s compiles for x86_64-sie-ps5 with clang %s" % ("clang, the recipe's arguments" if t.get("recipe") else
                                                                          "prospero-clang", LLVM_VERSION))
        return self.tool_notes

    def _native_tool(self):
        """PS5_Vulkan's ps5-native-tool runs here; if not (an older C library), it is built again from the folder's sources with this
        LLVM and the system's zlib"""
        tool = os.path.join(self.t["ps5_vulkan"], "build", "host", "ps5-native-tool" + (".exe" if os.name == "nt" else ""))
        r = subprocess.run([tool], capture_output=True, text=True) if os.path.isfile(tool) else None
        if r is not None and "ps5-native-tool" in r.stdout + r.stderr:
            self.ev.info("ok: ps5-native-tool runs")
            return tool
        native = os.path.join(self.t["ps5_vulkan"], "tooling", "native")
        if os.name == "nt":
            raise BuildError("toolchain", "ps5-native-tool.exe does not run (%s): unpack the builder's zip again"
                             % ((r.stderr.strip()[-200:] if r else "missing")))
        self.ev.info("ps5-native-tool does not run here (%s): building it from %s" % ((r.stderr.strip()[-200:] if r else "missing"), native))
        os.makedirs(os.path.dirname(tool), exist_ok=True)
        self._run([os.path.join(self.llvm["bindir"], "clang++"), "-std=c++20", "-O2", "-static-libstdc++", "-static-libgcc"] +
                  [os.path.join(native, n) for n in ("native_app_builder.cpp", "self_container.cpp", "elf_object.cpp", "sce_module_writer.cpp")] +
                  ["-lz", "-o", tool], os.path.join(self.logs or self.o.work or ".", "native-tool.log"),
                  "building ps5-native-tool (needs zlib's headers: zlib1g-dev)")
        return tool

    def _need_check(self):
        if self.env is None:
            self.check()

    def _need_work(self):
        if not (self.o.nup and self.o.out):
            raise BuildError("options", "the game file (.nup) and the output folder are needed")
        os.makedirs(self.logs, exist_ok=True)
        free = shutil.disk_usage(self.o.work).free / 2 ** 30
        if free < 8:
            raise BuildError("disk", "the work folder's disk has %.1f GB free: the build wants ~8 GB (the translated C 1.5 GB, objects, "
                             "the link, the title)" % free)
        return free

    # ---- 1. the game's program, from the .nup alone ----

    def read(self):
        self._need_check()
        check_nup(self.o.nup)   # (before anything is written: a refused file leaves no folder behind)
        self._need_work()
        return self._step(1, self._read)

    def _read(self):
        o, work = self.o, self.o.work
        tid, ver = check_nup(o.nup)
        self.ev.info("the .nup: %s, title %016X v%d (the supported one)" % (os.path.basename(o.nup), tid, ver))
        exefs = os.path.join(work, "exefs")
        st = os.stat(o.nup)
        stamp = "%s %d %d" % (os.path.basename(o.nup), st.st_size, int(st.st_mtime))
        mark = os.path.join(work, "nup", ".read-from")
        if os.path.isfile(os.path.join(work, "nup", "exefs", "main")) and os.path.isfile(mark) and open(mark).read() == stamp and \
                (o.no_prompts or os.path.isdir(os.path.join(work, "nup", "romfs-icons"))):
            self.ev.info("read before from this .nup: not read again")
        else:
            shutil.rmtree(os.path.join(work, "nup"), ignore_errors=True)
            self.ev.progress(None, "the game's program")
            self._run([o.python, "-I", "-u", os.path.join(NX2, "nup.py"), o.nup, "--only", "exefs", "--extract",
                       os.path.join(work, "nup")], os.path.join(self.logs, "1-read.log"), "reading the .nup")
            if not o.no_prompts:
                self.ev.progress(0.5, "the button icons' files")
                self._run([o.python, "-I", "-c", EXTRACT_PROMPTS, NX2, o.nup, os.path.join(work, "nup", "romfs-icons")] +
                          list(PROMPT_FILES), os.path.join(self.logs, "1-read.log"), "reading the button icons' game files")
            with open(mark, "w") as f:
                f.write(stamp)
        shutil.rmtree(exefs, ignore_errors=True)
        shutil.copytree(os.path.join(work, "nup", "exefs"), exefs)
        for name in sorted(os.listdir(exefs)):
            if name == "main.npdm" or name.endswith((".part", ".plain")):
                continue
            with open(os.path.join(exefs, name), "rb") as f:
                if f.read(4) != b"NSO0":
                    raise BuildError("nup-wrong-game", "%s in the game's program is not a game module" % name)
        with open(os.path.join(exefs, "main"), "rb") as f:
            bid = f.read(0x60)[0x40:0x60].hex()
        want = supported_build()
        if not bid.startswith(want):
            raise BuildError("nup-wrong-game", "the game's program is build %s; Grapnel is made from build %s (%s)" % (bid[:8], want, GAME_NAME))
        self.ev.info("the game's program: build %s (the supported one)" % bid[:16])
        self.exefs, self.bid = exefs, bid
        self.romfs = None if o.no_prompts else os.path.join(work, "nup", "romfs-icons")
        self.ev.progress(1.0, "read")

    # ---- 2. the translation ----

    def lift_jobs(self):
        """The translation's processes: the option, else by the memory free (one process 4.1 GiB, each further ~0.85 GiB, measured
        with 1 and 8 on 23/11/2026), at most 8 and the cores less one"""
        if self.o.lift_jobs:
            return self.o.lift_jobs
        free = mem_available_gb() or 0
        return max(1, min(8, cpu_count() - 1, int((free - 2 - 4.1) / 0.85) + 1))

    def translate(self):
        if self.exefs is None:
            self.read()
        with Lock(self.o.lock, self.ev):
            return self._step(2, self._translate)

    def _translate(self):
        work = self.o.work
        gen = os.path.join(work, "sa", "gen")
        image = os.path.join(work, "sa", "image", "image.bin")
        key = json.dumps({"exefs": tree_sha1(self.exefs), "tools": describe(), "env": LIFT_ENV, "profile": tree_sha1(PROFILE)},
                         sort_keys=True)
        mark = os.path.join(work, "sa", "grapnel-lift-key.txt")   # (beside gen/, not in it: gen/ stays the lift's files alone)
        if os.path.isfile(mark) and open(mark).read() == key and os.path.isfile(os.path.join(gen, "a64_table.c")) and os.path.isfile(image):
            self.ev.info("translated before from this program with these tools: not translated again")
            return gen
        if os.path.isfile(mark):
            os.remove(mark)
        os.makedirs(os.path.join(work, "image"), exist_ok=True)
        shutil.copyfile(os.path.join(PROFILE, "hot_funcs.txt"), os.path.join(work, "hot_funcs.txt"))
        for f in ("chunks.txt", "hot_pieces.txt"):   # (where the C files are cut, and the hot pieces kept: the port's own builds')
            shutil.copyfile(os.path.join(PROFILE, f), os.path.join(work, "image", f))
        if os.path.isdir(gen):   # (an earlier lift's C goes first: a file this one does not write would be compiled with it)
            for f in os.listdir(gen):
                if f.startswith("code_") and f.endswith((".c", ".c.whole", ".part")):
                    os.remove(os.path.join(gen, f))
        jobs = self.lift_jobs()
        self.ev.info("the translation: %d process%s" % (jobs, "" if jobs == 1 else "es"))
        env = dict(self.env, OOT_WORK=work, OOT_EXEFS=self.exefs, A64_HOT_FUNCS=os.path.join(work, "hot_funcs.txt"),
                   LONGSHOT_LIFT_JOBS=str(jobs), **LIFT_ENV)
        self.ev.progress(None, "translating")
        self._run([self.o.python, "-I", "-B", "-u", os.path.join(NX2, "lift.py"), "--standalone", "--write", "--exefs", self.exefs,
                   "--work", work], os.path.join(self.logs, "2-lift.log"), "the translation", env=env)
        self.ev.progress(0.9, "splitting the hot functions")
        self._run([self.o.python, "-I", "-B", "-u", os.path.join(NX2, "split_hot.py"), gen], os.path.join(self.logs, "2-lift.log"),
                  "the hot functions' split", env=env)
        if not os.path.isfile(os.path.join(gen, "a64_table.c")) or not os.path.isfile(image):
            raise BuildError("lift-incomplete", "the translation ended without its files: if the system stopped it for want of memory, "
                             "close other programs or give fewer translation processes (1), and build again")
        with open(mark, "w") as f:
            f.write(key)
        self.ev.progress(1.0, "translated")
        return gen

    # ---- 3. the compile, the link, the eboot ----

    def compile(self):
        if self.exefs is None:
            self.read()
        with Lock(self.o.lock, self.ev):
            return self._step(3, self._compile)

    def _compile(self):
        work, seen = self.o.work, {}
        progress = CompileProgress(os.path.join(work, "sa", "gen"))

        def on_line(line):
            f = progress.line(line)
            if f is not None:
                self.ev.progress(f, "compiling")
            m = re.search(r"mapped at load \(flexible memory\): ([0-9.]+) MiB", line)
            if m:
                seen["mapped"] = float(m.group(1))
            m = re.search(r"kernel\.flexibleMemorySize: (\d+) MiB", line)
            if m:
                seen["flexible"] = int(m.group(1))
            m = re.search(r"the eboot's imports: (.*) \(", line)
            if m:
                seen["imports"] = m.group(1)
            m = TITLE_LINE.search(line)
            if m:
                seen["title_id"], seen["title"], seen["eboot_bytes"] = m.group(1), m.group(2).rstrip(","), int(m.group(3))
            m = re.match(r"compiled in (\d+)s, (\d+) failed", line)
            if m:
                seen["compile_s"], seen["failed"] = int(m.group(1)), int(m.group(2))
                self.ev.progress(0.99, "linking and signing")
        env = dict(self.env, OOT_WORK=work, HOOKSHOT_PS5_FLEXIBLE_MB=str(FLEXIBLE_MB))
        if self.o.memory:
            env["LONGSHOT_MEM_LIMIT_GB"] = str(self.o.memory)
        llvm_mark, llvm_now = os.path.join(work, "sa", "grapnel-llvm.txt"), llvm_stamp(self.llvm)
        if objects_from_another_llvm(work, llvm_mark, llvm_now):
            self.ev.info("the objects here were compiled by another LLVM (or a builder that did not say which): compiled again")
            for d in (os.path.join(work, "sa", "build-ps5"), os.path.join(work, "objcache")):
                shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.dirname(llvm_mark), exist_ok=True)
        with open(llvm_mark, "w", encoding="utf-8") as f:   # (written before: a build stopped halfway keeps its objects for the next)
            f.write(llvm_now)
        self.ev.progress(0.0, "compiling")
        self._run([self.o.python, "-I", "-B", "-u", os.path.join(NX2, "build.py"), "--standalone", "--jobs", str(self.o.jobs)],
                  os.path.join(self.logs, "3-compile.log"), "the compile and link", on_line, env=env)
        if "title" not in seen or not os.path.isfile(os.path.join(seen["title"], "eboot.bin")):
            raise BuildError("compile-incomplete", "the build ended without a title folder", log=os.path.join(self.logs, "3-compile.log"))
        if seen.get("flexible") != FLEXIBLE_MB:
            raise BuildError("flexible", "the build's param.json says %s MiB of flexible memory, not %d" % (seen.get("flexible"), FLEXIBLE_MB))
        # (the shader translator's stamp the renderer was compiled with: the program caches the console keeps are good for it alone)
        import translator_files
        seen["shader_stamp_source"] = translator_files.stamp(os.path.join(ROOT, "gpu", "vk"))
        cc = os.path.join(work, "sa", "build-ps5", "compile_commands.json")
        m = re.search(r"LONGSHOT_SHADER_STAMP=0x([0-9a-f]{16})ull", open(cc, encoding="utf-8").read()) if os.path.isfile(cc) else None
        seen["shader_stamp"] = m.group(1) if m else None
        if seen["shader_stamp"] != seen["shader_stamp_source"]:
            raise BuildError("shader-stamp", "the renderer was compiled with the shader stamp %s, not its sources' %s"
                             % (seen["shader_stamp"], seen["shader_stamp_source"]))
        surface = os.path.join(work, "sa", "build-ps5", "ps5", "link-game", "surface.txt")
        rows = [l.split() for l in open(surface)] if os.path.isfile(surface) else []
        seen["imports_total"] = len(rows)
        bad = [r[1] for r in rows if r and r[0] in ("null", "missing")]
        if not rows or bad:
            raise BuildError("imports", "the eboot imports what no module a title loads exports: %s" % " ".join(bad[:10] or ["(no surface.txt)"]))
        self.ev.info("eboot: %d imports (%s), none null; shader stamp %s" % (seen["imports_total"], seen.get("imports"), seen["shader_stamp"]))
        self.seen = seen
        self.ev.progress(1.0, "compiled")
        return seen

    # ---- 4. the folders ----

    def lay_out(self):
        if not self.seen:
            raise BuildError("order", "lay_out needs compile first")
        return self._step(4, self._lay_out)

    def _lay_out(self):
        seen, out = self.seen, self.o.out
        title_id = seen["title_id"]
        if title_id != toolchain_ps5.TITLE_ID:
            raise BuildError("title", "the build made %s, not %s" % (title_id, toolchain_ps5.TITLE_ID))
        mapped = seen.get("mapped")
        if mapped is None:
            raise BuildError("flexible", "the build did not say how much of the eboot is mapped at load: the flexible memory cannot be checked")
        left = FLEXIBLE_MB - mapped - FLEXIBLE_AT_MAIN_MB
        self.ev.info("flexible memory: %d MiB in param.json, %.1f MiB of segments, ~%.1f MiB at main: ~%.0f MiB free at start (the memory "
                     "watch stops the title under %d)" % (FLEXIBLE_MB, mapped, FLEXIBLE_AT_MAIN_MB, left, FLEXIBLE_FLOOR_MB))
        if left < FLEXIBLE_FLOOR_MB:
            raise BuildError("flexible", "this eboot leaves ~%.0f MiB of flexible memory at start, under the memory watch's %d: the title "
                             "would close at once" % (left, FLEXIBLE_FLOOR_MB))
        title = os.path.join(out, "homebrew", title_id)
        if os.path.isdir(title):
            shutil.rmtree(title)
        shutil.copytree(seen["title"], title)
        art = self.o.title_art or ART
        self.art = []
        for f in TITLE_ART:
            src = os.path.join(art, f)
            if f == "pic1.dds" and not os.path.isfile(src):
                src = os.path.join(art, "pic0.dds")
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(title, "sce_sys", f))
                self.art.append(f)
        self.data_name = data_folder_name()
        data = os.path.join(out, "data", self.data_name)
        if os.path.isdir(data):
            shutil.rmtree(data)
        os.makedirs(data)
        for f in DATA_ART:
            if os.path.isfile(os.path.join(art, f)):
                shutil.copyfile(os.path.join(art, f), os.path.join(data, f))
                self.art.append(f)
            elif f == "hookshot_menu_fond.rgba" and os.path.isfile(os.path.join(art, MENU_PNG)):
                sys.path.insert(0, os.path.join(ROOT, "scripts", "ps5"))
                try:
                    import menu_fond
                finally:
                    sys.path.pop(0)
                w, h, rgba = menu_fond.read_picture(os.path.join(art, MENU_PNG))
                menu_fond.write(w, h, rgba, os.path.join(data, f), menu_fond.panel_of(MENU_PANEL))
                self.art.append(f + " (from " + MENU_PNG + ")")
        self.ev.info("the title's images: %s from %s" % (", ".join(self.art) or "none", art))
        shutil.copyfile(os.path.join(self.o.work, "sa", "image", "image.bin"), os.path.join(data, "image.bin"))
        shutil.copyfile(os.path.join(DATA, "hookshot_env.txt"), os.path.join(data, name_prefix() + "_env.txt"))
        shutil.copyfile(os.path.join(DATA, "skip.txt"), os.path.join(data, "skip.txt"))
        self.title, self.data = title, data
        return title, data

    # ---- 5. the button icons ----

    def icons(self):
        if self.data is None:
            raise BuildError("order", "icons needs lay_out first")
        return self._step(5, self._icons)

    def _icons(self):
        if self.o.no_prompts or not self.romfs:
            self.ev.info("no button icons (the game shows the Switch's)")
            return None
        dst = os.path.join(self.data, "prompts")
        shutil.rmtree(dst, ignore_errors=True)
        self.ev.progress(None, "drawing the button icons")
        # (SOURCE_DATE_EPOCH: fontTools writes this date in the font's head table, not the time of the build, so every player's
        # External.bfotf is the same file: without it two builds differed by 6 bytes)
        self._run([self.o.prompt_python, "-I", "-B", "-u", os.path.join(NX2, "prompt_assets.py"), "--romfs", self.romfs, "--out", dst],
                  os.path.join(self.logs, "5-prompts.log"), "the button icons", env=dict(self.env, SOURCE_DATE_EPOCH=FONT_DATE))
        # (the keyboard face would be chosen on the console when no controller is named yet; the key is the PC game's cache check)
        shutil.rmtree(os.path.join(dst, "keyboard"), ignore_errors=True)
        if os.path.isfile(os.path.join(dst, ".prompt_key")):
            os.remove(os.path.join(dst, ".prompt_key"))
        self.icons_dir = dst
        return dst

    # ---- 6. the shaders' lists, for the preparation on this PC (lot 82, lot 83) ----

    def shaders(self):
        if self.data is None:
            raise BuildError("order", "shaders needs lay_out first")
        return self._step(6, self._shaders)

    def _shaders(self):
        """<work>/shaders/<prefix>_shaders.bin (the census, from the player's romfs: ~290 MB of its 26 GB read out of the .nup) and
        <prefix>_pipeline_states_shipped.bin (from this checkout's lists), for step 7. The console no longer reads them (lot 83:
        it gets the driver's cache step 7 makes)"""
        o, work, data = self.o, self.o.work, self.data
        census_name, list_name = name_prefix() + "_shaders.bin", name_prefix() + "_pipeline_states_shipped.bin"
        for n in (census_name, list_name):
            if os.path.isfile(os.path.join(data, n)):
                os.remove(os.path.join(data, n))
        if o.no_shaders:
            self.ev.info("no list of the game's shaders (--no-shaders), no preparation: the game compiles them as it plays")
            self.shader_files = None
            return None
        # (the game files the census reads, out of the .nup once: a second build from the same .nup reads them again only if gone)
        romfs = os.path.join(work, "nup", "romfs-shaders")
        st = os.stat(o.nup)
        stamp = "%s %d %d" % (os.path.basename(o.nup), st.st_size, int(st.st_mtime))
        mark = os.path.join(romfs, ".read-from")
        if os.path.isfile(mark) and open(mark).read() == stamp:
            self.ev.info("the shaders' game files read before from this .nup: not read again")
        else:
            shutil.rmtree(romfs, ignore_errors=True)
            self.ev.progress(0.0, "reading the shaders' game files")
            self._run([o.python, "-I", "-c", EXTRACT_CENSUS, NX2, o.nup, romfs, ",".join(CENSUS_FOLDERS), CENSUS_DICT],
                      os.path.join(self.logs, "6-shaders.log"), "reading the shaders' game files")
            with open(mark, "w") as f:
                f.write(stamp)
        # (the census: made again when its game files or nx2/shader_census.py changed)
        census = os.path.join(work, "shaders", census_name)
        key = json.dumps({"romfs": stamp, "census": sha256(os.path.join(NX2, "shader_census.py"))}, sort_keys=True)
        kmark = census + ".key"
        if os.path.isfile(census) and os.path.isfile(kmark) and open(kmark).read() == key:
            self.ev.info("the census of the game's shaders made before from these files: not made again")
        else:
            os.makedirs(os.path.dirname(census), exist_ok=True)
            if os.path.isfile(kmark):
                os.remove(kmark)
            self.ev.progress(0.3, "listing the game's shaders")
            self._run([o.python, "-I", "-B", "-u", os.path.join(NX2, "shader_census.py"), romfs, "--out", census],
                      os.path.join(self.logs, "6-shaders.log"), "the census of the game's shaders")
            if not os.path.isfile(census):
                raise BuildError("census", "the census of the game's shaders ended without its file", log=os.path.join(self.logs, "6-shaders.log"))
            with open(kmark, "w") as f:
                f.write(key)
        facts = census_facts(census)
        if (facts["programs"], facts["mesh"], facts["compute"]) != (CENSUS_PROGRAMS, CENSUS_MESH, CENSUS_COMPUTE):
            raise BuildError("census-wrong", "the census holds %d programs (%d mesh, %d compute), not this game's %d (%d, %d): a romfs "
                             "file was not read, or another update" % (facts["programs"], facts["mesh"], facts["compute"],
                                                                       CENSUS_PROGRAMS, CENSUS_MESH, CENSUS_COMPUTE))
        self.ev.info("the census of the game's shaders: %d programs (%d mesh, %d of them with a task stage; %d compute), %d stages, %.0f MB"
                     % (facts["programs"], facts["mesh"], facts["mesh_task"], facts["compute"], facts["stages"], facts["bytes"] / 1e6))
        self.ev.progress(0.9, "the pipelines' list")
        states = os.path.join(work, "shaders", list_name)
        records, left = make_state_list(states)
        if not records:
            raise BuildError("states", "the pipelines' list is empty: this checkout has no assets/pipeline_states")
        self.ev.info("the pipelines' list: %d pipelines from the public lists (%d of the converted ones left out), no game file" % (records, left))
        self.shader_files = {"census": facts, "states": records, "states_left_out": left,
                             "census_path": census, "states_path": states, "states_sha256": sha256(states)}
        self.ev.progress(1.0, "listed")
        return self.shader_files

    # ---- 7. the console driver's shader cache, made on this PC (lot 83) ----

    def prepare(self):
        if self.title is None:
            raise BuildError("order", "prepare needs lay_out first")
        if self.shader_files is None and not self.o.no_shaders:
            raise BuildError("order", "prepare needs shaders first")
        with Lock(self.o.lock, self.ev):
            return self._step(7, self._prepare)

    def _prepare(self):
        """The console's own preparation (gpu/vk/vk_draw.cpp console_prepare_*), run here by the toolchain's prepare/ on every core
        into <work>/prepare/radv-shader-cache, as the console's driver keeps its cache; checked (each part of the database whole, at
        least PREP_MIN_ENTRIES entries, the progress file finished for the console's driver, nothing left out, and a second pass
        that rebuilds everything and finds it all there, compiling nothing), then put in the title folder (radv-shader-cache/: the
        driver's cache folder on the console, /app0/radv-shader-cache) with its progress file, also beside the game for the launch
        screen's check (<prefix>_prepare_progress.txt). Again from where it stopped when a run stops short of memory"""
        o, work = self.o, self.o.work
        dst_cache = os.path.join(self.title, "radv-shader-cache")
        progress_name = name_prefix() + "_prepare_progress.txt"
        for p in (dst_cache, os.path.join(self.data, progress_name)):
            if os.path.isdir(p):
                shutil.rmtree(p)
            elif os.path.isfile(p):
                os.remove(p)
        if o.no_shaders or o.no_prepare or self.prep_tool is None:
            self.ev.info("no shader preparation (%s): the console compiles the game's shaders as you play"
                         % ("--no-shaders" if o.no_shaders else "--no-prepare"))
            self.prepared = None
            return None
        tool, sf = self.prep_tool, self.shader_files
        prep = os.path.join(work, "prepare")
        run, cache = os.path.join(prep, "run"), os.path.join(prep, "radv-shader-cache")
        log = os.path.join(self.logs, "7-prepare.log")
        workers = o.prepare_workers or cpu_count()
        # (what the cache is made from: another tool, census, list, settings or skip list is a cache made again from nothing)
        key = json.dumps({"tool": sha256(os.path.join(tool, "prepare.json")), "census": open(sf["census_path"] + ".key").read(),
                          "states": sf["states_sha256"], "env": sha256(os.path.join(DATA, "hookshot_env.txt")),
                          "skip": sha256(os.path.join(DATA, "skip.txt"))}, sort_keys=True)
        kmark = os.path.join(prep, "made-from.json")
        progress = os.path.join(cache, "hookshot_prepare.txt")
        if not (os.path.isfile(kmark) and open(kmark).read() == key):
            shutil.rmtree(prep, ignore_errors=True)
        os.makedirs(run, exist_ok=True)
        os.makedirs(cache, exist_ok=True)
        with open(kmark, "w") as f:
            f.write(key)
        # (the run folder: the host and the renderer beside the files they read under the PC build's names (longshot_*))
        for n in PREP_PROGRAMS:
            shutil.copy2(os.path.join(tool, n), os.path.join(run, n))
        for src, n in ((sf["census_path"], "longshot_shaders.bin"), (sf["states_path"], "longshot_pipeline_states_shipped.bin"),
                       (os.path.join(DATA, "skip.txt"), "skip.txt")):
            dst = os.path.join(run, n)
            if os.path.lexists(dst):
                os.remove(dst)
            if os.name == "nt":   # (windows: a copy; a symbolic link asks for a privilege, and Wine's made nothing at all)
                shutil.copyfile(src, dst)
                continue
            try:
                os.symlink(src, dst)
            except OSError:
                shutil.copyfile(src, dst)
        env = prepare_env(tool, run, cache, workers)
        before = read_progress(progress)
        checked = os.path.join(prep, "checked.json")
        if before.get("finished") == "1" and os.path.isfile(checked) and open(checked).read() == key:
            self.ev.info("the shaders were prepared and checked before from the same files: not again (%s)" % prep)
            self.prepared = json.load(open(os.path.join(prep, "prepared.json")))
            self.prepared["made_before"] = True
            shutil.copytree(cache, dst_cache, ignore=shutil.ignore_patterns("*.tmp", "index"))
            shutil.copyfile(progress, os.path.join(self.data, progress_name))
            return self.prepared
        if before.get("finished") == "1":
            self.ev.info("the shaders were prepared before from the same files: not again, checked now (%s)" % prep)
        else:
            self.ev.info("preparing the shaders on %d cores: the console's own preparation, into the driver's cache as the console keeps "
                         "it (about 15 minutes on a recent 8-core PC, up to 45 on a small one; the memory free decides how many compile at once)" % workers)
            self._prepare_runs(run, env, log, "the preparation", 0.0, 0.85)
        made = read_progress(progress)
        facts = mesa_db_facts(cache)
        if made.get("finished") != "1":
            raise BuildError("prepare-unfinished", "the shaders' preparation did not finish (see its log)", log=log)
        if facts["bad"] or facts["entries"] < PREP_MIN_ENTRIES:
            raise BuildError("prepare-cache", "the driver's cache the preparation made is not whole: %d entries in %d parts%s"
                             % (facts["entries"], facts["parts"], "; " + ", ".join(facts["bad"]) if facts["bad"] else ""), log=log)
        if made.get("skip"):
            self.ev.info("note: %d of the preparation's items ended the run twice and were left out: the console compiles them when "
                         "first drawn" % len(made["skip"]))
        made_seconds, made_skip = float(made.get("seconds", 0)), len(made.get("skip", []))
        # (the second pass: every item again in a new process; all of it is found in the cache, nothing is compiled)
        second = []
        for attempt in range(2):
            n0 = facts["entries"]
            self._prepare_runs(run, dict(env, HOOKSHOT_PS5_PREPARE="force"), log, "the check", 0.85 + 0.07 * attempt, 0.92 + 0.07 * attempt)
            facts = mesa_db_facts(cache)
            second.append(facts["entries"] - n0)
            if facts["entries"] == n0:
                break
        else:
            raise BuildError("prepare-unstable", "the shaders' preparation is not settled: two passes after it still compiled %d and %d "
                             "pipelines" % tuple(second), log=log)
        if facts["bad"]:
            raise BuildError("prepare-cache", "the driver's cache is not whole after the check: %s" % ", ".join(facts["bad"]), log=log)
        made = read_progress(progress)
        if made.get("finished") != "1":
            raise BuildError("prepare-unfinished", "the check pass did not finish", log=log)
        # (into the title folder: the database's parts and the progress file; a copy of that beside the game too, where the launch
        # screen reads it when no MESA_SHADER_CACHE_DIR names another cache folder: gpu/vk/vk_draw.cpp prep_progress_path)
        shutil.copytree(cache, dst_cache, ignore=shutil.ignore_patterns("*.tmp", "index"))
        shutil.copyfile(progress, os.path.join(self.data, progress_name))
        keys, digest = mesa_db_digest(cache)
        self.prepared = {"entries": facts["entries"], "keys": keys, "digest": digest, "parts": facts["parts"], "bytes": facts["bytes"], "workers": workers,
                         "translator": made.get("translator"), "driver": made.get("driver"), "items": int(made.get("total", 0)),
                         "left_to_play": made_skip, "seconds": made_seconds, "check_seconds": float(made.get("seconds", 0)),
                         "second_pass_new": second, "made_before": before.get("finished") == "1"}
        with open(os.path.join(prep, "prepared.json"), "w") as f:
            json.dump(self.prepared, f, indent=1)
        with open(checked, "w") as f:
            f.write(key)
        self.ev.info("the driver's shader cache: %d entries in %d parts, %.0f MB, made in %.0f s on %d cores; the second pass found "
                     "everything%s; %d keys, content %s" % (facts["entries"], facts["parts"], facts["bytes"] / 1e6, self.prepared["seconds"],
                                                         workers, "" if not second[0] else " (after adding %d)" % second[0], keys, digest))
        return self.prepared

    def _prepare_runs(self, run, env, log, what, lo, hi):
        """The preparation's runs until it finishes: a run that stops short of memory (13) or on a compile that never ends (14)
        is run again, and goes on where it stopped; the progress from its lines every 10 s"""
        last = {}
        estimate = PrepEstimate()

        def on_line(line):
            estimate.start_line(line)
            m = PREP_PROGRESS.search(line)
            if m:
                pct, left = int(m.group(6)), m.group(7)
                mine = estimate.at(int(m.group(1)), int(m.group(2) or 0), int(m.group(4)), read_left(left))
                if mine:   # (the builder's own estimate, by the steps' measured shares: see PrepEstimate)
                    pct, left = int(100 * mine[0]), say_left(mine[1])
                last.update(free=int(m.group(9)), pct=pct)
                self.ev.progress(lo + (hi - lo) * pct / 100, "%s: %s, %d%% (%s left; %s MiB free)" % (what, m.group(2) and "step %s of 3"
                                                                                                      % m.group(2), pct, left, m.group(9)))
        for attempt in range(PREP_TRIES):
            try:
                self._run([os.path.join(run, PREP_PROGRAMS[0]), "--prepare-shaders"], log, what, on_line=on_line, env=env, cwd=run,
                          idle=PREP_SILENT_S)
                return
            except BuildError as e:
                code = int(re.search(r"exit (-?\d+)", e.message).group(1)) if re.search(r"exit (-?\d+)", e.message) else -1
                if (code not in (13, 14, SILENT_EXIT) and not crashed(code)) or attempt == PREP_TRIES - 1:
                    raise BuildError("prepare-failed", "%s on this PC ended (exit %d)%s" % (
                        what, code, ": short of memory again and again; free some memory or use --prepare-workers" if code == 13 else ""),
                        e.details, log)
                self.ev.info("%s stopped (%s): again, from where it stopped" % (
                    what, {13: "the PC's memory ran short", 14: "a compile did not end"}.get(code, "it said nothing for %d minutes"
                                                                                              % (PREP_SILENT_S // 60))
                    if not crashed(code) else "it crashed, code %#x" % (code & 0xFFFFFFFF)))
                time.sleep(5)

    # ---- 8. the notes ----

    def write_notes(self):
        if self.title is None:
            raise BuildError("order", "write_notes needs lay_out first")
        return self._step(8, self._notes)

    def _notes(self):
        out, title, data, seen = self.o.out, self.title, self.data, self.seen
        title_id = os.path.basename(title)
        env_name = name_prefix() + "_env.txt"
        icons = self.icons_dir is not None
        prepared = self.prepared is not None
        progress_name = name_prefix() + "_prepare_progress.txt"
        fill = {"v": VERSION, "id": title_id, "data": self.data_name, "env": env_name,
                "icons": (", prompts/" if icons else "") + (", " + progress_name if prepared else ""),
                "prep_en": (PREP_EN if prepared else NO_PREP_EN) % {"id": title_id},
                "prep_fr": (PREP_FR if prepared else NO_PREP_FR) % {"id": title_id}}
        with open(os.path.join(out, "INSTALL.en.txt"), "w", encoding="utf-8") as f:
            f.write(INSTALL_EN % dict(fill, icons2=(", prompts/" if icons else "") + (" and " + progress_name if prepared else "")))
        with open(os.path.join(out, "INSTALL.fr.txt"), "w", encoding="utf-8") as f:
            f.write(INSTALL_FR % dict(fill, icons2=(", prompts/" if icons else "") + (" et " + progress_name if prepared else "")))
        with open(os.path.join(out, "PRIVATE.txt"), "w", encoding="utf-8") as f:
            f.write("PRIVATE / PRIVÉ: homebrew/%s/eboot.bin holds the game's code translated and compiled, data/%s/image.bin is the game's\n"
                    "program, data/%s/prompts/ holds the game's font, textures and layouts, homebrew/%s/radv-shader-cache/ the game's\n"
                    "shaders compiled (and the work folder's shaders/ their code). Never share them: only copy them to your own console.\n"
                    "Ne les partagez jamais : copiez-les seulement sur votre console.\n"
                    % (title_id, self.data_name, self.data_name, title_id))
        files = {}
        for base in (title, data):
            for d, _, fs in os.walk(base):
                for n in sorted(fs):
                    p = os.path.join(d, n)
                    e = {"bytes": os.path.getsize(p)}
                    if n in ("eboot.bin", "image.bin", "param.json", "hookshot_prepare.txt", progress_name):
                        e.update(crc32=crc32(p), sha256=sha256(p))
                    files[os.path.relpath(p, out)] = e
        manifest = {"builder": "Grapnel builder %s" % VERSION, "checkout": describe(), "game_build": self.bid[:16],
                    "game": "%016X v%d" % (GAME_TITLE_ID, GAME_VERSION), "title_id": title_id, "title_name": toolchain_ps5.TITLE_NAME,
                    "data_folder": "/data/" + self.data_name, "flexible_mb": seen.get("flexible"), "mapped_mb": seen.get("mapped"),
                    "imports": seen.get("imports"), "imports_total": seen.get("imports_total"), "shader_stamp": seen.get("shader_stamp"),
                    "title_art": self.art, "lift_env": LIFT_ENV, "llvm": self.tool_notes.get("llvm"),
                    "shaders": None if self.shader_files is None else {k: v for k, v in self.shader_files.items() if not k.endswith("_path")},
                    "prepared": self.prepared,
                    "toolchain": self.tool_notes.get("toolchain"),
                    "steps": [{"step": s, "seconds": round(t), "peak_pss_mib": None if p is None else round(p)} for s, t, p in self.times],
                    "files": files}
        with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        return manifest

    # ---- all of it ----

    def run(self):
        quiet_crashes()
        self.check()
        check_nup(self.o.nup)
        free = self._need_work()
        self.ev.info("work folder %s (%.0f GB free); compiles at once %d%s" % (self.o.work, free, self.o.jobs,
                                                                             ", memory %.1f GB" % self.o.memory if self.o.memory else ""))
        self.read()
        self.translate()
        self.compile()
        self.lay_out()
        self.icons()
        self.shaders()
        self.prepare()
        manifest = self.write_notes()
        eb = os.path.join(self.title, "eboot.bin")
        self.ev.info("eboot.bin: %d bytes, CRC32 %d, sha256 %s; image.bin CRC32 %d" % (
            os.path.getsize(eb), crc32(eb), sha256(eb), crc32(os.path.join(self.data, "image.bin"))))
        if self.prepared:
            self.ev.info("radv-shader-cache: %d entries, %d bytes" % (self.prepared["entries"], self.prepared["bytes"]))
        return manifest


INSTALL_EN = """Grapnel %(v)s for PS5: what to copy to your console
=====================================================

PRIVATE: everything here was made from your own copy of the game. Never share, upload or post it: only copy it to your own
console. (grapnel-work/, or your work folder, is private too: delete it when you no longer need it.)

The console needs a jailbreak with an ELF loader and /data access (the port is played on firmware up to 12.60). Tested on a base
PS5 only: the PS5 Pro was never tried.

1. homebrew/%(id)s  ->  /data/homebrew/%(id)s   (FTP or your file manager), then register the title (your homebrew tools'
   "register" or "install from folder"). It shows on the home screen as "Grapnel".
2. data/%(data)s  ->  /data/%(data)s
   (image.bin, %(env)s, skip.txt%(icons)s).
   If your console already has /data/hookshot from a Hookshot port, do NOT make /data/%(data)s: copy
   image.bin%(icons2)s
   into /data/hookshot instead (keep your own %(env)s there). Grapnel's first start renames /data/hookshot to /data/%(data)s,
   saves and all (nothing copied); a /data/%(data)s made beforehand would be taken instead, and your saves left behind.
3. Your game file  ->  /data/%(data)s/game.nup   (the same .nup you built from, as it is: 25.6 GB).
4. Start Grapnel from the home screen.
%(prep_en)s
eboot.bin and image.bin go together: after every new build, copy both again.
"""

PREP_EN = """   The game's shaders are prepared already: homebrew/%(id)s/radv-shader-cache/ is the console driver's shader cache, made
   on this PC, so that the game does not stop to compile them as you play. The console prepares nothing: the launch screen
   only checks the cache at start (its SHADERS page says "Ready"); a pipeline no list knows is still compiled when first
   drawn. After a new build, delete the console's old /data/homebrew/%(id)s/radv-shader-cache before you copy the title
   folder.
"""
NO_PREP_EN = """   The first starts compile the game's shaders as you play (stutters, and more memory) until they are all in the driver's cache,
   which stays in the title folder.
"""
PREP_FR = """   Les shaders du jeu sont déjà préparés : homebrew/%(id)s/radv-shader-cache/ est le cache de shaders du pilote de la
   console, fait sur ce PC, pour que le jeu ne s'arrête pas pour les compiler en jouant. La console ne prépare rien : l'écran
   de lancement vérifie seulement le cache au démarrage (sa page SHADERS dit « Prêts ») ; un pipeline qu'aucune liste ne
   connaît est encore compilé à sa première apparition. Après une nouvelle construction, effacez l'ancien
   /data/homebrew/%(id)s/radv-shader-cache de la console avant de copier le dossier du titre.
"""
NO_PREP_FR = """   Les premiers lancements compilent les shaders du jeu pendant que vous jouez (saccades, et plus de mémoire), jusqu'à ce
   qu'ils soient tous dans le cache du pilote, qui reste dans le dossier du titre.
"""

INSTALL_FR = """Grapnel %(v)s pour PS5 : ce qu'il faut copier sur la console
=============================================================

PRIVÉ : tout ce qui est ici a été fait à partir de votre propre copie du jeu. Ne le partagez, ne l'envoyez et ne le publiez
jamais : copiez-le seulement sur votre console. (grapnel-work/, ou votre dossier de travail, est privé aussi : effacez-le quand
vous n'en avez plus besoin.)

La console doit être débridée, avec un chargeur d'ELF et l'accès à /data (le portage est joué jusqu'au firmware 12.60). Essayé
sur une PS5 standard seulement : la PS5 Pro n'a jamais été essayée.

1. homebrew/%(id)s  ->  /data/homebrew/%(id)s   (FTP ou votre gestionnaire de fichiers), puis enregistrez le titre
   (« register » ou « installer depuis un dossier » de vos outils homebrew). Il apparaît à l'accueil sous le nom « Grapnel ».
2. data/%(data)s  ->  /data/%(data)s
   (image.bin, %(env)s, skip.txt%(icons)s).
   Si la console a déjà /data/hookshot d'un portage Hookshot, NE créez PAS /data/%(data)s : copiez plutôt
   image.bin%(icons2)s
   dans /data/hookshot (gardez-y votre %(env)s). Le premier lancement de Grapnel renomme /data/hookshot en /data/%(data)s,
   sauvegardes comprises (rien n'est copié) ; un /data/%(data)s créé avant serait pris à sa place, et vos sauvegardes laissées.
3. Votre fichier du jeu  ->  /data/%(data)s/game.nup   (le même .nup que celui de la construction, tel quel : 25,6 Go).
4. Lancez Grapnel depuis l'accueil.
%(prep_fr)s
eboot.bin et image.bin vont ensemble : après chaque nouvelle construction, recopiez les deux.
"""
