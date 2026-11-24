#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel's shader preparation for the PC (linux-x64), built once on a developer's PC and shipped in the toolchain's prepare/
folder (make_toolchain.py --prepare): what the builder runs to make the console driver's shader cache on the player's PC, so that
the console compiles nothing. **No game code and no game file in it**: the game's shaders come from the player's own .nup at
build time (the builder's census, step 6).

    python3 -I scripts/grapnel-builder/make_prepare_tool.py --out <folder> [--work W] [--jobs N] [--cpm D] [--sdk-cxx D]
        [--libcxx D] [--mesa-build D] [--mesa-source D] [--vulkan-loader F]

    <folder>/longshot            the console tree's Linux host with none of the game's code: host/, the guest model's runtime,
                                 opus, zstd (nx2/build.py --runtime), the lift's tables empty (scripts/ps5/boot/boot_nogame.c) and
                                 an empty stand-in for each symbol of the lifted code (a --prepare-shaders start reaches none);
                                 the lift's two headers written as the lifter writes them for no code (a64_rtgen, fpx_header)
    <folder>/longshot_vk.so      this checkout's renderer for Linux with the console build's C++ library layout (libc++ 18.1's
                                 headers from the payload SDK, as the eboot; Debian's libc++ 19 at run time): the same
                                 translation, so the same SPIR-V, as the console's (build/cache-pilote, build/merge3)
    <folder>/lib/                libvulkan_radeon.so: our PS5_Mesa RADV built for Linux on the PS5 winsys's host model (the
                                 console's driver, same build ID: its shader cache keys are the console's), libdrm.so.2; LLVM's
                                 libc++.so.1, libc++abi.so.1, libunwind.so.1; libvulkan.so.1 (the Khronos loader)
    <folder>/icd.json            the loader's manifest for lib/libvulkan_radeon.so (a path relative to it)
    <folder>/prepare.json        what it is: the shader stamp, the driver's build ID, the revisions, each file's size and SHA-256
    <folder>/LICENSES/           each part's licence; NOTICE.md

Debug information is dropped from every library (it names this PC's folders) and no file may name this PC's home folder.
The builder runs it as `longshot --prepare-shaders` with RX_VK_CONSOLE_PREPARE_RUN=1 (gpu/vk/vk_main.cpp): the console's own
preparation (lot 82) to its end, on every core of the PC (grapnel_core.py, Builder.prepare).
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import toolchain_ps5  # noqa: E402
import translator_files  # noqa: E402

VERSION = "0.2"   # (0.2: --system windows-x64)
# (the lift's settings the builder's lift runs with, grapnel_core.LIFT_ENV: the headers are written with them)
LIFT_ENV = {"A64_HOT_CHUNK": "1", "A64_FASTFP": "0", "A64_FPX_NC": "1", "A64_VFPX_ELEM": "hot"}
LIBCXX = ("libc++.so.1", "libc++abi.so.1", "libunwind.so.1")
# (windows-x64: llvm-mingw's clang, run on this Linux PC, for Windows: the GNU ABI on the UCRT, its C++ runtime linked in)
WIN_TARGET = ["--target=x86_64-w64-mingw32", "-fms-extensions", "-D_CRT_SECURE_NO_WARNINGS"]
WIN_HOST_LIBS = ["-lwinhttp", "-luser32", "-lwinmm", "-ladvapi32", "-lole32", "-lshell32", "-lgdi32", "-ldbghelp"]   # (nx2/build.py HOST_LIBS)


def windows(a):
    return a.system == "windows-x64"


def cc(a, cxx=False):
    """The C (C++) compiler for the tool's system, with its target"""
    if windows(a):
        return [os.path.join(a.mingw, "bin", "clang++" if cxx else "clang")] + WIN_TARGET
    return [os.path.join(a.llvm, "clang++" if cxx else "clang")]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(cmd, **kw):
    print("$ " + " ".join(cmd)[:300], flush=True)
    subprocess.run(cmd, check=True, **kw)


def rev(folder):
    r = subprocess.run(["git", "-C", folder, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


# ---- the lift's headers, for no game code ----
GEN_HEADERS = r'''
import os, sys
root, gen = sys.argv[1], sys.argv[2]
sys.path[:0] = [os.path.join(root, "nx2")]
import kit  # noqa: F401  (a64-kit's lift folder on the path)
import fastfp, suyu_rt_fixes, a64_lift, a64_rtgen
suyu_rt_fixes.install()
os.makedirs(gen, exist_ok=True)
a64_rtgen.write(gen)
text = a64_lift.fpx_header(set())
if a64_lift.local_flags():
    text += a64_lift.FCMPK_HEADER
if fastfp.fastfp_mode():
    text += fastfp.FASTFP_HEADER
with open(os.path.join(gen, "a64_fpx.h"), "w", newline="\n") as f:
    f.write(text)
'''


# ---- the host ----
def build_host(a, work, out):
    """nx2/build.py --runtime for Linux in a work folder of its own, then the link of build/merge3/build-host-linux.sh"""
    # (RX_MARCH: the players' PCs, not this one's -march=native, which nx2/build.py takes by itself on an x86-64-v3 CPU: the tool
    # made on an Alder Lake was built for Alder Lake; only MOVBE came of it, which v3 has, but another PC could give more)
    env = dict(os.environ, OOT_WORK=work, RX_CPM=a.cpm, RX_MARCH="x86-64-v3", **LIFT_ENV)
    env.pop("RX_TARGET", None)   # (the PC build, not the console's)
    if windows(a):   # (nx2/build.py's cross build for Windows: RX_TARGET=windows with llvm-mingw, a64-kit tools/toolchain.py)
        env.update(RX_TARGET="windows", LONGSHOT_MINGW=a.mingw)
    gen = os.path.join(work, "sa", "gen")
    run([sys.executable, "-I", "-B", "-c", GEN_HEADERS, ROOT, gen], env=env)
    # (nn_video.c fails there: no FFmpeg headers on a PC without its development files. It is compiled below against the
    # console build's stand-in headers, as the console's title has it)
    r = subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, "nx2", "build.py"), "--runtime", "--jobs", str(a.jobs)],
                       env=env, cwd=ROOT, capture_output=True, text=True)
    with open(os.path.join(work, "runtime-build.log"), "w") as f:
        f.write(r.stdout + r.stderr)
    obj = os.path.join(work, "sa", "build", "obj")
    if not os.path.isdir(obj) or not [n for n in os.listdir(obj) if n.startswith("host_")]:
        sys.exit("make_prepare_tool: nx2/build.py --runtime made no host objects: %s" % os.path.join(work, "runtime-build.log"))
    failed = re.findall(r"(\d+) failed", r.stdout + r.stderr)
    print("runtime: %s" % ((r.stdout + r.stderr).strip().splitlines() or ["?"])[-1], flush=True)
    if failed and int(failed[-1]) > 1:
        sys.exit("make_prepare_tool: nx2/build.py --runtime: %s failed (nn_video.c alone is expected)" % failed[-1])
    host = os.path.join(ROOT, "host")
    posix = os.path.join(host, "posix")
    mine = os.path.join(work, "host")
    os.makedirs(mine, exist_ok=True)
    clang = cc(a)
    # (windows: host/posix's headers stand in for Windows' on Linux, so only for #include "...": MinGW has the real ones)
    psx = ["-iquote" + posix] if windows(a) else ["-I" + posix, "-D_GNU_SOURCE"]
    fl = ["-march=x86-64-v3", "-ffp-contract=off"] + psx + ["-I" + host, "-I" + os.path.join(work, "sa", "build", "rt64"),
          "-I" + gen, "-Wno-everything", "-O2", "-g0", "-mcx16", "-DA64_POLLING", "-DA64_WRITE_TRACK",
          "-I" + os.path.join(ROOT, "third_party", "opus", "include"), "-ffile-prefix-map=%s/=" % ROOT, "-ffile-prefix-map=%s/=" % work]
    nf = os.path.join(posix, "ps5_noffmpeg")
    run(clang + fl + ["-I" + nf, "-c", os.path.join(host, "nn_video.c"), "-o", os.path.join(mine, "host_nn_video.o")])
    # (ps5_stubs.c's FFmpeg part: from its #ifndef HOOKSHOT_PS5_FFMPEG to the #endif that closes it, nested #if counted)
    lines, on, depth = [], False, 0
    for line in open(os.path.join(posix, "ps5_stubs.c"), encoding="utf-8"):
        if not on:
            on, depth = line.startswith("#ifndef HOOKSHOT_PS5_FFMPEG"), 0
            continue
        if line.startswith("#if"):
            depth += 1
        elif line.startswith("#endif"):
            if not depth:
                on = False
                continue
            depth -= 1
        lines.append(line)
    with open(os.path.join(mine, "ffmpeg_stubs.c"), "w") as f:
        f.write('#include "../hos.h"\n#include <stdlib.h>\n' + "".join(lines))
    with open(os.path.join(ROOT, "scripts", "ps5", "boot", "boot_nogame.c"), encoding="utf-8") as f:
        nogame = re.sub(r"(?m)^#ifdef __PROSPERO__", "#if 1", f.read())
    with open(os.path.join(mine, "nogame.c"), "w") as f:
        f.write(nogame)
    run(clang + fl + ["-I" + nf] + psx[:1] + ["-c", os.path.join(mine, "ffmpeg_stubs.c"), "-o", os.path.join(mine, "ffmpeg_stubs.o")])
    run(clang + fl + ["-c", os.path.join(mine, "nogame.c"), "-o", os.path.join(mine, "nogame.o")])
    objs = [os.path.join(obj, n) for n in sorted(os.listdir(obj))
            if n.endswith(".o") and n != "host_nn_video.o" and not n.startswith("vk_")]   # (the renderer: longshot_vk.so)
    objs += [os.path.join(mine, n) for n in ("host_nn_video.o", "ffmpeg_stubs.o", "nogame.o")]
    exe = os.path.join(out, "longshot.exe" if windows(a) else "longshot")

    def link(extra):
        if windows(a):   # (as the Windows game's: static, its C++ runtime inside, an 8 MB main stack, the game's manifest)
            # (nx2/win_manifest.py: UTF-8 as the process's code page, for a player's folder named in any script, and the segment
            # heap, on which the preparation's workers do not queue in the driver's compiler)
            sys.path.insert(0, os.path.join(ROOT, "nx2"))
            import win_manifest
            res = win_manifest.write(os.path.join(mine, "longshot_manifest.res"))
            return subprocess.run(clang + ["-fuse-ld=lld", "-static", "-o", exe, "-Wl,--gc-sections", "-Wl,--stack,8388608"] +
                                  objs + extra + [res] + WIN_HOST_LIBS + ["-lc++", "-lunwind", "-s"], capture_output=True, text=True)
        return subprocess.run(clang + ["-fuse-ld=lld", "-o", exe, "-Wl,--gc-sections", "-Wl,--export-dynamic-symbol=hos_posix_*"] +
                              objs + extra + ["-lpthread", "-ldl", "-lm", "-s"], capture_output=True, text=True)
    r = link([])
    if r.returncode:
        # (what the lifted code would define: empty, 64 zero bytes each, never reached in a --prepare-shaders start)
        # (not a system library's import, __imp_*: that would be a library missing from the link)
        undefined = sorted(set(u for u in re.findall(r"undefined symbol: ([A-Za-z_0-9]+)", r.stderr) if not u.startswith("__imp_")))
        if not undefined:
            sys.exit("make_prepare_tool: the host's link failed:\n" + r.stderr[-3000:])
        with open(os.path.join(mine, "lift_empty.c"), "w") as f:
            f.write("/* the lifted code's symbols, empty: a --prepare-shaders start reaches none of them */\n")
            f.writelines("__attribute__((aligned(16))) char %s[64];\n" % s for s in undefined)
        run(clang + ["-O0", "-g0", "-fno-common", "-c", os.path.join(mine, "lift_empty.c"), "-o", os.path.join(mine, "lift_empty.o")])
        r = link([os.path.join(mine, "lift_empty.o")])
        if r.returncode:
            sys.exit("make_prepare_tool: the host's link failed:\n" + r.stderr[-3000:])
        print("host: linked with %d empty stand-ins for the lifted code" % len(undefined), flush=True)
    return exe


# ---- the renderer ----
def newest(d):
    return os.path.join(d, sorted(os.listdir(d))[-1])


def renderer_sources(gpu, imgui, zbic, mihawk, gen_dir):
    """build/lot82/build-vk-linux.sh's: nx2/build.py vk_sources() (eden, sirit/src, the back end, suyu's texture decoders), then
    Dear ImGui, the fonts, zstd, PS5_MihawkGen's generator"""
    vk = os.path.join(gpu, "vk")
    srcs = []
    for top in (os.path.join(vk, "eden"), os.path.join(vk, "sirit", "src")):
        for d, _, fs in os.walk(top):
            srcs += [os.path.join(d, n) for n in fs if n.endswith(".cpp")]
    srcs += [os.path.join(vk, n) for n in os.listdir(vk) if n.endswith(".cpp") and n != "vk_ngx_msvc_rt.cpp"]
    srcs += [os.path.join(gpu, "third_party", "suyu", "video_core", "textures", n) for n in ("decoders.cpp", "astc.cpp")]
    srcs += [os.path.join(imgui, n) for n in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                                                  "backends/imgui_impl_vulkan.cpp")]
    srcs += [os.path.join(gen_dir, "ui_fonts.cpp"), os.path.join(zbic, "zstd.c")]
    if os.path.isdir(mihawk):
        srcs += [os.path.join(mihawk, "src", n + ".cpp") for n in ("fg_common", "fg_vulkan", "fg_neural", "fg_splat")]
    return sorted(srcs)


def stale(o, d, cmd):
    """Whether the object is to make again: missing, another command, or a file its depfile names newer than it"""
    if not os.path.exists(o) or not os.path.exists(d) or not os.path.exists(o + ".cmd"):
        return True
    if open(o + ".cmd").read() != cmd:
        return True
    t = os.path.getmtime(o)
    deps = open(d).read().replace("\\\n", " ").split(":", 1)[-1].split()
    return any(not os.path.exists(p) or os.path.getmtime(p) > t for p in deps)


def build_renderer(a, work, out):
    gpu = os.path.join(ROOT, "gpu")
    vk = os.path.join(gpu, "vk")
    posix = os.path.join(ROOT, "host", "posix")
    imgui = os.path.join(ROOT, "third_party", "imgui")
    zbic = os.path.join(ROOT, "third_party", "zbic")
    mihawk = os.path.join(ROOT, "third_party", "mihawkgen", "framegen")
    gen_dir = os.path.join(work, "gen")
    obj_dir = os.path.join(work, "obj")
    os.makedirs(gen_dir, exist_ok=True)
    os.makedirs(obj_dir, exist_ok=True)
    with open(os.path.join(a.sdk_cxx, "__config")) as f:
        if "define _LIBCPP_VERSION 180100" not in f.read():
            sys.exit("make_prepare_tool: no libc++ 18.1 headers in %s" % a.sdk_cxx)
    stamp = translator_files.stamp(vk)
    sys.path.insert(0, os.path.join(ROOT, "nx2"))
    import version
    import logo_rgba
    import ui_fonts
    logo_rgba.write(os.path.join(gen_dir, "longshot_logo.h"))
    ui_fonts.write(os.path.join(gen_dir, "ui_fonts.cpp"))
    version_def = version.define()
    vk_headers, spirv_headers, vma = (newest(os.path.join(a.cpm, n)) for n in ("vulkanheaders", "spirv-headers", "vulkanmemoryallocator"))
    maps = ["-ffile-prefix-map=%s/=" % ROOT, "-ffile-prefix-map=%s/=" % work, "-ffile-prefix-map=%s/=" % a.cpm,
            "-ffile-prefix-map=%s/=sdk/" % os.path.dirname(os.path.dirname(os.path.dirname(a.sdk_cxx)))]
    # (windows: no host/posix, whose headers stand in for Windows' on Linux; MinGW has the real ones)
    common = ["-march=x86-64-v3", "-mtune=znver2", "-ffp-contract=off", "-fno-omit-frame-pointer", "-ffunction-sections",
              "-fdata-sections"] + ([] if windows(a) else ["-fPIC", "-I" + posix, "-D_GNU_SOURCE"]) + ["-DHOOKSHOT_CACHE_BUILD"] + maps
    cxx = common + ["-nostdinc++", "-isystem", a.sdk_cxx] + \
        sum((["-include", h] for h in ("algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector")), []) + \
        ["-std=c++20", "-O2", "-g0", "-DNDEBUG", "-Wno-everything", "-DVK_NO_PROTOTYPES", "-DMKWVK_BUILDING", "-fexceptions",
         "-fcxx-exceptions", "-frtti", "-DMKWVK_INCLUDES=2",
         "-I" + os.path.join(vk, "compat"), "-I" + os.path.join(gpu, "compat"), "-I" + os.path.join(vk, "eden"),
         "-I" + os.path.join(gpu, "third_party", "suyu"), "-I" + os.path.join(vk, "sirit", "include"),
         "-I" + os.path.join(vk, "sirit", "src"), "-I" + os.path.join(spirv_headers, "include"),
         "-I" + os.path.join(spirv_headers, "include", "spirv", "unified1"), "-I" + os.path.join(vk_headers, "include"),
         "-I" + vk, "-I" + gen_dir, "-I" + os.path.join(vma, "include"), "-I" + zbic]
    imgui_fl = ["-I" + imgui, "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES", "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
                "-DIMGUI_DISABLE_WIN32_FUNCTIONS", "-DIMGUI_DISABLE_DEMO_WINDOWS", "-DIMGUI_DISABLE_DEBUG_TOOLS"]
    c_fl = common + ["-O2", "-g0", "-Wno-everything"]
    jobs = []
    for src in renderer_sources(gpu, imgui, zbic, mihawk, gen_dir) + ([] if windows(a) else [os.path.join(posix, "win32_posix.c")]):
        rel = os.path.relpath(src, gen_dir if src.startswith(gen_dir) else ROOT).replace(os.sep, "_")
        o = os.path.join(obj_dir, rel + ".o")
        extra = []
        # (the back end's own files: vk_internal.h names std::thread on Linux, which libc++ 18 does not bring in by itself)
        if os.path.dirname(src) == vk and src.endswith(".cpp"):
            extra += ["-include", "thread"]
        name = os.path.basename(src)
        if name == "vk_shader.cpp":
            extra.append("-DLONGSHOT_SHADER_STAMP=0x%sull" % stamp)
        elif name == "vk_menu.cpp":
            extra += [version_def] + imgui_fl
        elif name in ("vk_imgui.cpp", "vk_launcher.cpp") or src.startswith(imgui):
            extra += imgui_fl
        elif name == "vk_framegen.cpp" and os.path.isdir(mihawk):
            extra += ["-DHOOKSHOT_MIHAWKGEN", "-I" + os.path.join(mihawk, "include")]
        elif src.startswith(mihawk):
            extra += ["-include", os.path.join(vk, "mihawkgen_glue.h"), "-I" + os.path.join(mihawk, "include"), "-I" + os.path.join(mihawk, "src")]
        elif name == "zstd.c":
            extra += ["-DNDEBUG", "-fvisibility=hidden"]
        tool = cc(a, cxx=not src.endswith(".c"))
        cmd = tool + (c_fl if src.endswith(".c") else cxx) + extra + ["-MMD", "-MF", o + ".d", "-c", src, "-o", o]
        jobs.append((o, cmd))

    def one(job):
        o, cmd = job
        key = "\0".join(cmd)
        if not stale(o, o + ".d", key):
            return o, ""
        r = subprocess.run(["nice", "-n", "19"] + cmd, capture_output=True, text=True)
        if r.returncode:
            return o, r.stderr[-2000:] or "failed"
        with open(o + ".cmd", "w") as f:
            f.write(key)
        return o, ""
    made = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for o, err in pool.map(one, jobs):
            if err:
                sys.exit("make_prepare_tool: %s: %s" % (o, err))
            made += 1
    print("renderer: %d objects (shader stamp %s)" % (made, stamp), flush=True)
    so = os.path.join(out, "longshot_vk.dll" if windows(a) else "longshot_vk.so")
    rsp = os.path.join(work, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(o for o, _ in jobs))
    if windows(a):   # (as the Windows game's longshot_vk.dll: llvm-mingw's libc++ 19 inside, under the SDK's 18.1 headers, as on Linux)
        link = cc(a, cxx=True) + ["-fuse-ld=lld", "-static", "-shared", "-o", so, "-Wl,--gc-sections", "@" + rsp, "-luser32",
                                  "-lc++", "-lunwind", "-s"]
        r = subprocess.run(link, capture_output=True, text=True)
        if r.returncode:
            # (clang 19 for COFF names the default of an inline thread_local's weak init function, _ZTH*, after a function of its
            # object: two objects whose first function is the same inline one define the same .weak._ZTH*.default.* name. Both
            # are that one variable's initialisation, so either serves; nothing else may be defined twice)
            dup = re.findall(r"duplicate symbol: (\S+)", r.stderr)
            if not dup or any(not d.startswith(".weak._ZTH") for d in dup):
                sys.exit("make_prepare_tool: the renderer's link failed:\n" + r.stderr[-3000:])
            print("renderer: %d inline thread_local initialisers named twice (.weak._ZTH*.default.*): linked with the first of each"
                  % len(dup), flush=True)
            run(link + ["-Wl,--allow-multiple-definition"])
        return so, stamp
    run([os.path.join(a.llvm, "clang++"), "-fuse-ld=lld", "-shared", "-o", so, "-Wl,-Bsymbolic", "-Wl,--gc-sections", "@" + rsp,
         "-nostdlib++", "-L" + os.path.join(a.libcxx, "lib", "x86_64-linux-gnu"), "-Wl,-rpath,$ORIGIN/lib", "-lc++", "-lc++abi",
         "-ldl", "-lpthread", "-lm", "-s"])
    return so, stamp


# ---- the driver's libraries ----
def set_runpath(path, value):
    """DT_RUNPATH (or DT_RPATH) of an ELF64 little-endian file rewritten in place to value, its old string blanked: the string
    table is left as it is, so value must not be longer, and no other entry may point inside the old string"""
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1, path
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
        dyn = next(s for s in secs if s[1] == 6)   # SHT_DYNAMIC
        dynstr = secs[dyn[6]]                      # (its sh_link)
        str_off = dynstr[4]
        entries, at = [], dyn[4]
        while at < dyn[4] + dyn[5]:
            tag, val = struct.unpack_from("<qQ", data, at)
            entries.append((at, tag, val))
            if tag == 0:
                break
            at += 16
        runs = [(at, tag, val) for at, tag, val in entries if tag in (15, 29)]   # DT_RPATH, DT_RUNPATH
        if not runs:
            return False
        for at, tag, val in runs:
            start = str_off + val
            end = data.index(b"\0", start)
            old = bytes(data[start:end])
            # (any other dynamic string pointing inside the old one: symbols, versions, needed libraries)
            others = [v for a2, t2, v in entries if t2 in (1, 14) and a2 != at]
            dynsym = next((s for s in secs if s[1] == 11), None)
            if dynsym:
                for i in range(dynsym[5] // 24):
                    others.append(struct.unpack_from("<I", data, dynsym[4] + i * 24)[0])
            if any(val < v < val + len(old) for v in others):
                sys.exit("make_prepare_tool: %s: a dynamic string shares its run path's bytes" % path)
            if len(value) > len(old):
                sys.exit("make_prepare_tool: %s: run path %r longer than the old one" % (path, value))
            data[start:end] = value.encode() + b"\0" * (len(old) - len(value))
        f.seek(0)
        f.write(data)
    return True


def copy_lib(src, dst, llvm, runpath=None):
    shutil.copy2(os.path.realpath(src), dst)
    os.chmod(dst, 0o755)
    subprocess.run([os.path.join(llvm, "llvm-objcopy"), "--strip-debug", dst], check=True)
    if runpath is not None:
        set_runpath(dst, runpath)


def main():
    above = toolchain_ps5._above
    ap = argparse.ArgumentParser(description="Grapnel's shader preparation for the PC, for the toolchain (no game code)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", help="the build's own files (default <out>.work)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--cpm", default=above("wt", "ps5-port", "third_party-ps5", "cpm"))
    ap.add_argument("--sdk-cxx", default=above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk", "target",
                                               "include", "c++", "v1"), help="libc++ 18.1's headers (the payload SDK's)")
    ap.add_argument("--libcxx", default=above("probe", "build", "cache-test2", "libcxx19", "usr"),
                    help="Debian's libc++ 19 runtime, unpacked (libc++1-19, libc++abi1-19, libunwind-19 and their -dev)")
    ap.add_argument("--mesa-build", default=above("mesa-work", "game-build", "host-align"),
                    help="the PS5 RADV built for Linux on the PS5 winsys's host model (mesa-work/build-host-game.sh GAME=align)")
    ap.add_argument("--mesa-source", default=above("mesa-work", "game-align"))
    ap.add_argument("--libdrm-source", default=above("mesa-work", "taskfix-build", "src-host", "subprojects", "libdrm-2.4.133"))
    ap.add_argument("--vulkan-loader", default="/usr/lib/x86_64-linux-gnu/libvulkan.so.1")
    ap.add_argument("--llvm", default="/usr/lib/llvm-19/bin")
    ap.add_argument("--only", choices=("host", "renderer"), help="(development) build that part alone into <out>")
    ap.add_argument("--system", choices=("linux-x64", "windows-x64"), default="linux-x64",
                    help="the players' system: windows-x64 is cross-built here with llvm-mingw (--mingw) and the RADV of --mesa-build "
                         "built for Windows (vulkan_radeon.dll)")
    ap.add_argument("--mingw", default=above("build", "grapnel-windows", "mingw19", "llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64"),
                    help="(windows-x64) llvm-mingw's release for this Linux PC")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or out + ".work")
    for k in ("cpm", "sdk_cxx", "libcxx", "mesa_build", "mesa_source", "libdrm_source", "mingw"):
        setattr(a, k, os.path.abspath(getattr(a, k)))
    if a.only is None and os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out if windows(a) else os.path.join(out, "lib"), exist_ok=True)   # (windows: the DLLs beside the exe)
    os.makedirs(work, exist_ok=True)
    stamp = translator_files.stamp(os.path.join(ROOT, "gpu", "vk"))
    if a.only in (None, "host"):
        build_host(a, os.path.join(work, "host"), out)
    if a.only in (None, "renderer"):
        _, stamp = build_renderer(a, os.path.join(work, "renderer"), out)
    if a.only:
        return
    lib = os.path.join(out, "lib")
    build_id = open(os.path.join(a.mesa_build, ".radv-build-id")).read().strip() if os.path.exists(
        os.path.join(a.mesa_build, ".radv-build-id")) else ""
    if windows(a):
        copy_windows_driver(a, out)
    else:
        copy_linux_libs(a, lib)
    # ---- the licences ----
    lic = os.path.join(out, "LICENSES")
    os.makedirs(lic)
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(lic, "grapnel-LICENSE"))
    shutil.copy2(os.path.join(a.mesa_source, "docs", "license.rst"), os.path.join(lic, "mesa-license.rst"))
    if windows(a):
        windows_licences(a, out, lic)
    else:
        linux_licences(a, lic)
    finish(a, out, stamp, build_id)


def copy_linux_libs(a, lib):
    copy_lib(os.path.join(a.mesa_build, "src", "amd", "vulkan", "libvulkan_radeon.so"), os.path.join(lib, "libvulkan_radeon.so"),
             a.llvm, "$ORIGIN")
    copy_lib(os.path.join(a.mesa_build, "subprojects", "libdrm-2.4.133", "libdrm.so.2"), os.path.join(lib, "libdrm.so.2"), a.llvm)
    for n in LIBCXX:
        copy_lib(os.path.join(a.libcxx, "lib", "x86_64-linux-gnu", n), os.path.join(lib, n), a.llvm)
    copy_lib(a.vulkan_loader, os.path.join(lib, "libvulkan.so.1"), a.llvm)
    with open(os.path.join(os.path.dirname(lib), "icd.json"), "w") as f:
        json.dump({"file_format_version": "1.0.1", "ICD": {"library_path": "./lib/libvulkan_radeon.so", "api_version": "1.4.335"}},
                  f, indent=4)


def copy_windows_driver(a, out):
    """vulkan_radeon.dll beside the exe, without its debug information, and what it imports beyond Windows' own (none but the
    UCRT and KERNEL32 when zlib is linked in; a libz-1.dll otherwise). The builder opens it directly (GRAPNEL_VK_ICD,
    gpu/vk/vk_main.cpp), as the console's title has its driver; icd.json is for a loader all the same"""
    bin_ = os.path.join(a.mingw, "bin")
    dst = os.path.join(out, "vulkan_radeon.dll")
    shutil.copy2(os.path.join(a.mesa_build, "src", "amd", "vulkan", "vulkan_radeon.dll"), dst)
    subprocess.run([os.path.join(bin_, "llvm-objcopy"), "--strip-debug", dst], check=True)
    imports = subprocess.run([os.path.join(bin_, "llvm-readobj"), "--coff-imports", dst], capture_output=True, text=True,
                             check=True).stdout
    for name in sorted(set(re.findall(r"Name: (\S+\.dll)", imports))):
        low = name.lower()
        if low.startswith("api-ms-win-") or low in ("kernel32.dll", "user32.dll", "advapi32.dll", "ole32.dll", "shell32.dll",
                                                    "ws2_32.dll", "bcrypt.dll", "ntdll.dll", "dbghelp.dll"):
            continue
        found = [os.path.join(d, name) for d, _, fs in os.walk(os.path.join(a.mesa_build, "subprojects")) if name in fs]
        if not found:
            sys.exit("make_prepare_tool: vulkan_radeon.dll imports %s, not found in %s" % (name, a.mesa_build))
        shutil.copy2(found[0], os.path.join(out, name))
        subprocess.run([os.path.join(bin_, "llvm-objcopy"), "--strip-debug", os.path.join(out, name)], check=True)
    with open(os.path.join(out, "icd.json"), "w") as f:
        json.dump({"file_format_version": "1.0.1", "ICD": {"library_path": ".\\vulkan_radeon.dll", "api_version": "1.4.335"}},
                  f, indent=4)


def windows_licences(a, out, lic):
    """LLVM's (libc++, libunwind and compiler-rt's builtins are linked in) and MinGW-w64's runtime and winpthreads (linked into
    the driver), as llvm-mingw ships them; zlib's when its DLL is there"""
    shutil.copy2(os.path.join(a.mingw, "LICENSE.TXT"), os.path.join(lic, "llvm-LICENSE.txt"))
    share = os.path.join(a.mingw, "x86_64-w64-mingw32", "share", "mingw32")
    for n in ("COPYING.MinGW-w64-runtime.txt", "COPYING.winpthreads.txt"):
        if os.path.exists(os.path.join(share, n)):
            shutil.copy2(os.path.join(share, n), os.path.join(lic, n))
    if os.path.exists(os.path.join(out, "libz-1.dll")):
        zl = [os.path.join(d, "LICENSE") for d, _, fs in os.walk(os.path.join(a.mesa_build, "..", "mesa-win", "subprojects"))
              if "zlib" in d and "LICENSE" in fs]
        if zl:
            shutil.copy2(zl[0], os.path.join(lic, "zlib-LICENSE.txt"))


def linux_licences(a, lic):
    with open(os.path.join(a.libdrm_source, "xf86drm.c"), encoding="utf-8") as f:
        head = f.read().split("*/", 2)
    with open(os.path.join(lic, "libdrm-license.txt"), "w") as f:
        f.write("libdrm 2.4.133 (lib/libdrm.so.2): its licence, from xf86drm.c\n\n" + head[1].lstrip("\n").split("/*", 1)[-1] + "*/\n")
    for pkg in ("libc++1-19", "libc++abi1-19", "libunwind-19"):
        src = os.path.join(a.libcxx, "share", "doc", pkg, "copyright")
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(lic, pkg + "-copyright"))
    loader_doc = "/usr/share/doc/libvulkan1/copyright"
    if os.path.exists(loader_doc):
        shutil.copy2(loader_doc, os.path.join(lic, "libvulkan1-copyright"))


def finish(a, out, stamp, build_id):
    # ---- nothing of this PC in it ----
    home = os.path.expanduser("~").encode()
    named = [os.path.relpath(os.path.join(d, n), out) for d, _, fs in os.walk(out) for n in fs
             if home in open(os.path.join(d, n), "rb").read()]
    if named:
        sys.exit("make_prepare_tool: %d files name this PC's home folder, not made: %s" % (len(named), " ".join(named)))
    # ---- what it is ----
    files = {}
    for d, _, fs in os.walk(out):
        for n in fs:
            p = os.path.join(d, n)
            files[os.path.relpath(p, out).replace(os.sep, "/")] = {"bytes": os.path.getsize(p), "sha256": sha256(p)}
    vk_tree = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD:gpu/vk"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "gpu", "host", "nx2", "scripts/ps5"],
                           capture_output=True, text=True).stdout.strip()
    info = {"format": 1, "name": "grapnel-prepare", "version": VERSION, "system": a.system, "made": datetime.date.today().isoformat(),
            "shader_stamp": stamp, "driver_build_id": build_id, "tree": rev(ROOT) + ("+changes" if dirty else ""),
            "gpu_vk_tree": vk_tree if not dirty else "", "mesa": rev(a.mesa_source),
            "run": ("longshot.exe --prepare-shaders, RX_VK_CONSOLE_PREPARE_RUN=1, GRAPNEL_VK_ICD=vulkan_radeon.dll" if windows(a) else
                    "longshot --prepare-shaders, RX_VK_CONSOLE_PREPARE_RUN=1, LD_LIBRARY_PATH=lib, VK_DRIVER_FILES=icd.json"),
            "files": files}
    with open(os.path.join(out, "prepare.json"), "w") as f:
        json.dump(info, f, indent=1, sort_keys=True)
    total = sum(v["bytes"] for v in files.values())
    print("prepare tool: %s, %d files, %.0f MB, shader stamp %s, driver %s" % (out, len(files), total / 1e6, stamp, build_id[:12]))


if __name__ == "__main__":
    main()
