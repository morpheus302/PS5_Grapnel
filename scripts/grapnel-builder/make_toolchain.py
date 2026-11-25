#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel's console toolchain for the builder, laid out in one folder from a developer's PC: everything the build of the PS5
title needs that is neither the game nor in this repository, and no game file at all.

    python3 -I scripts/grapnel-builder/make_toolchain.py --out <folder> [--system linux-x64|windows-x64] [--sdk D]
        [--ps5-vulkan D] [--radv-archive F] [--cpm D] [--dav1d D] [--dav1d-include D] [--llvm D] [--mingw D] [--prepare D]

    <folder>/toolchain.json   what is in it: each file's size and SHA-256, where each part came from (revisions)
    <folder>/NOTICE.md        each part's licence and where its source is
    <folder>/sdk/             the PS5 payload SDK (ps5-payload-dev/sdk v0.42 with mihawk-99's PS5_PayloadSDK platform layer):
                              the prospero-* wrappers, target/ (headers, module stubs, libc, libc++, libps5platform.a)
    <folder>/ps5_vulkan/      PS5_Vulkan's parts the eboot's link uses: tools/radv-link.sh, tooling/native (the CRT, the
                              symbol map, the native tool's sources), tooling/psbc/ps5-pie-unwind.ld, tooling/prospero-clang18,
                              the AGC link stubs, runtime/libc.prx, and build/host/ps5-native-tool rebuilt here with its C++
                              library and zlib inside (it then needs only glibc 2.34)
    <folder>/radv/            the RADV archive the port's eboot links (our PS5_Mesa branch) and its PROVENANCE.txt
    <folder>/cpm/             the renderer's headers (Vulkan-Headers, SPIRV-Headers, VulkanMemoryAllocator)
    <folder>/dav1d/           dav1d built for the console: src/libdav1d.a, include/ (generated), include-src/ (its public headers)
    <folder>/llvm-rt/         (0.2) compiler-rt's builtins for x86-64 (an ELF archive: the link's __emutls_get_address), the
                              ones of the LLVM 19.1.7 every tested eboot was linked with, so the player's LLVM need not have them
    <folder>/ps5_recipe.json  (0.2) what the SDK's prospero-* wrappers, tooling/prospero-clang18 and tools/radv-link.sh give the
                              LLVM tools, recorded here by running those scripts once (record_recipe): with it the build runs
                              clang, ld.lld and llvm-ar directly, no bash (nx2/toolchain_ps5.py, direct mode), on Linux and Windows
    <folder>/ftpsrv/          (0.3) ftpsrv-ps5.elf, ps5-payload-dev/ftpsrv (GPL-3.0-or-later) at FTPSRV_COMMIT, compiled here with
                              the SDK's prospero-clang (--ftpsrv-source: its checkout): the FTP server the assistant's step 6
                              sends to the console's ELF loader when no FTP server answers; with its LICENSE, its sources and
                              PROVENANCE.txt
    <folder>/prepare/         (0.2) the shaders' preparation for the PC, made by make_prepare_tool.py (--prepare: its folder):
                              the console tree's Linux host and this release's renderer with no game code, the console driver's
                              PC model and their libraries (the builder's step 7 runs it on the player's PC)

--system windows-x64 makes the same folder for Windows, on Linux: ps5-native-tool.exe cross-built with llvm-mingw (--mingw) and
zlib 1.3.2's sources, the SDK's symbolic links made copies (a zip for Windows has none). Its prepare/ (--prepare) must be a
preparation built for Windows.

The builder takes it with --toolchain <folder> (scripts/grapnel-builder/grapnel_build.py). Defaults: the places
nx2/toolchain_ps5.py finds them on the port's PC (above this checkout).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import toolchain_ps5  # noqa: E402

VERSION = "0.3.2"   # (0.2: prepare/, the shaders' preparation for the PC; 0.3: ps5_recipe.json and llvm-rt/, the build without bash,
                  # ftpsrv/ for step 6, prepare/ for Windows too: Grapnel 0.2's kit;
                    # 0.3.1: the same parts, prepare/ made again for Grapnel 0.2.1's renderer;
                    # 0.3.2: the same parts, prepare/ 0.3, whose gate on a PC never stops for memory: Grapnel 0.2.3)
NATIVE_SOURCES = ("native_app_builder.cpp", "self_container.cpp", "elf_object.cpp", "sce_module_writer.cpp")
PS5_VULKAN_FILES = ("LICENSE", "NOTICE.md", "tools/radv-link.sh", "tooling/prospero-clang18", "tooling/psbc/ps5-pie-unwind.ld",
                    "vendor/ps5/sdk/stubs/agc_canary_link_stub.c", "vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c",
                    "runtime/libc.prx", "runtime/libc.prx.sha256", "runtime/README.md")
GENERATED = ("ps5_vulkan/build/radv-platform-local.map",)   # (written by radv-link.sh at every link: not in the manifest)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def rev(folder):
    r = subprocess.run(["git", "-C", folder, "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


# (ps5-payload-dev/ftpsrv: John Törnblom's FTP server for jailbroken PS4s and PS5s, GPL-3.0-or-later; sha256.c, Brad Conte's,
# public domain. Its PS5 payload is these sources, as its Makefile.ps5's ftpsrv-ps5.elf)
FTPSRV_COMMIT = "92072f2"
FTPSRV_SOURCES = ("srv.c", "cmd.c", "notify.c", "io.c", "self.c", "sha256.c", "main-prospero.c", "cmd-prospero.c",
                  "self-prospero.c", "io-freebsd.c")


def build_ftpsrv(a, dst):
    """ftpsrv-ps5.elf compiled with the SDK's prospero-clang from the checkout at FTPSRV_COMMIT, stripped, no path of this PC
    in it; its sources (the files compiled and their headers), LICENSE and PROVENANCE.txt beside it"""
    src = os.path.abspath(a.ftpsrv_source)
    head = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", src, "status", "--porcelain"], capture_output=True, text=True).stdout.strip()
    if not head.startswith(FTPSRV_COMMIT) or dirty:
        sys.exit("make_toolchain: %s is not ftpsrv at %s unchanged (%s%s)" % (src, FTPSRV_COMMIT, head[:12], ", changed" if dirty else ""))
    os.makedirs(os.path.join(dst, "src"))
    elf = os.path.join(dst, "ftpsrv-ps5.elf")
    sdk_bin = os.path.join(a.sdk, "bin")
    subprocess.run([os.path.join(sdk_bin, "prospero-clang"), "-Wall", "-Werror", '-DVERSION_TAG="%s"' % FTPSRV_COMMIT, "-I" + src,
                    '-DTITLE_ID="BREW10001"', "-ffile-prefix-map=%s=ftpsrv" % src, "-o", elf] +
                   [os.path.join(src, n) for n in FTPSRV_SOURCES], check=True, cwd=dst)
    subprocess.run([os.path.join(sdk_bin, "prospero-strip"), elf], check=True)
    for n in FTPSRV_SOURCES + tuple(n for n in sorted(os.listdir(src)) if n.endswith(".h")) + ("README.md", "Makefile.ps5"):
        shutil.copy2(os.path.join(src, n), os.path.join(dst, "src", n))
    shutil.copy2(os.path.join(src, "LICENSE"), os.path.join(dst, "LICENSE"))
    with open(os.path.join(dst, "PROVENANCE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("ftpsrv-ps5.elf: github.com/ps5-payload-dev/ftpsrv at %s (John Törnblom; GPL-3.0-or-later, LICENSE; sha256.c by "
                "Brad Conte, public domain), compiled by Grapnel's make_toolchain.py with the payload SDK's prospero-clang "
                "from the sources in src/ (those of its Makefile.ps5's ftpsrv-ps5.elf), then stripped. The FTP server the "
                "builder's step 6 sends to the console's ELF loader (port 9021) when none answers on port 2121.\n" % head)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


BROKER_FLAGS = ["-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]   # (nx2/toolchain_ps5.py link_title: the broker's compile)
RECORDER = """#!/bin/sh
# (make_toolchain record_recipe) a stand-in for an LLVM tool: what the script before it ran, written down, one argument a line
for a in "$@"; do printf '%s\\n' "$a"; done > "$GRAPNEL_RECORD"
"""


def _record(cmd, env):
    """The arguments a script hands its LLVM tool: the script run with the recorder in LLVM's place"""
    rec = env["GRAPNEL_RECORD"]
    if os.path.exists(rec):
        os.remove(rec)
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode or not os.path.exists(rec):
        sys.exit("make_toolchain: %s did not run its tool (%d): %s" % (" ".join(cmd[:2]), r.returncode, r.stderr[-800:]))
    with open(rec, encoding="utf-8") as f:
        return f.read().split("\n")[:-1]


def _around(argv, mine):
    """argv split around the arguments the caller gave (mine, in one run): the script's own before them, and after"""
    for i in range(len(argv) - len(mine) + 1):
        if argv[i:i + len(mine)] == mine:
            return argv[:i], argv[i + len(mine):]
    sys.exit("make_toolchain: the script did not pass %s on as given: %s" % (mine, argv))


def record_recipe(out, llvm, builtins, radv):
    """What the SDK's wrappers (prospero-clang, prospero-clang++, prospero-lld), PS5_Vulkan's tooling/prospero-clang18 and
    tools/radv-link.sh give the LLVM tools, recorded by running them (from this folder) with a recorder standing in for clang and
    ld.lld; and the link clang's driver makes of the storage broker's command (-###). The folder's paths written {sdk}, {vk},
    {radv}, {builtins}; nx2/toolchain_ps5.py's direct mode fills them in"""
    import shlex
    import tempfile
    sdk, vk = os.path.join(out, "sdk"), os.path.join(out, "ps5_vulkan")
    places = [(os.path.realpath(builtins), "{builtins}"), (builtins, "{builtins}"), (os.path.realpath(radv), "{radv}"),
              (radv, "{radv}"), (os.path.realpath(sdk), "{sdk}"), (os.path.realpath(vk), "{vk}"), (sdk, "{sdk}"), (vk, "{vk}")]

    def generic(args):
        res = []
        for a in args:
            for real, name in places:
                a = a.replace(real, name)
            if os.path.expanduser("~") in a or "/tmp/" in a:
                sys.exit("make_toolchain: the recipe would name this PC: %s" % a)
            res.append(a)
        return res
    with tempfile.TemporaryDirectory() as tmp:
        fake = os.path.join(tmp, "llvm")
        os.makedirs(fake)
        for n in ("clang", "clang++", "ld.lld"):
            with open(os.path.join(fake, n), "w") as f:
                f.write(RECORDER)
            os.chmod(os.path.join(fake, n), 0o755)
        with open(os.path.join(fake, "llvm-config"), "w") as f:   # (the wrappers ask it the version and the bin folder)
            f.write('#!/bin/sh\ncase "$1" in --bindir) echo "%s";; --version) echo 19.1.7;; *) exit 1;; esac\n' % fake)
        os.chmod(os.path.join(fake, "llvm-config"), 0o755)
        env = dict(os.environ, LLVM_CONFIG=os.path.join(fake, "llvm-config"), GRAPNEL_RECORD=os.path.join(tmp, "argv.txt"),
                   PATH=fake + os.pathsep + "/usr/bin:/bin", PS5_PAYLOAD_SDK=sdk, PS5_CLANG=os.path.join(fake, "clang"))
        mine = ["-c", "SOURCE.c", "-o", "OBJECT.o"]
        compile_ = {}
        for lang, script in (("c", "prospero-clang"), ("c++", "prospero-clang++")):
            before, after = _around(_record([os.path.join(sdk, "bin", script)] + mine, env), mine)
            compile_[lang] = {"before": generic(before), "after": generic(after)}
        before, after = _around(_record(["sh", os.path.join(vk, "tooling", "prospero-clang18")] + mine, env), mine)
        if after:
            sys.exit("make_toolchain: prospero-clang18 added arguments after the command's: %s" % after)
        crt = {"before": generic(before)}
        lld = {}
        for case, mine in (("pie+script", ["-o", "OUT.elf", "IN.o"]), ("pie", ["-T", "SCRIPT.ld", "-o", "OUT.elf", "IN.o"]),
                           ("nopie+script", ["--shared", "-o", "OUT.so", "IN.o"])):
            before, after = _around(_record([os.path.join(sdk, "bin", "prospero-lld")] + mine, env), mine)
            lld[case] = {"before": generic(before), "after": generic(after)}
        # (the broker: clang's driver compiles and links it in one command; what it hands prospero-lld, with LLVM 19.1.7's own clang)
        real = dict(os.environ, LLVM_CONFIG=os.path.join(llvm, "llvm-config"))
        src = os.path.join(ROOT, "host", "posix", "ps5_broker_payload.c")
        r = subprocess.run([os.path.join(sdk, "bin", "prospero-clang")] + BROKER_FLAGS + ["-I" + os.path.join(ROOT, "host", "posix"),
                            "-o", os.path.join(tmp, "OUT.elf"), src, "-###"], env=real, capture_output=True, text=True)
        lines = [l for l in r.stderr.splitlines() if l.startswith(' "')]
        if r.returncode or len(lines) != 2 or not shlex.split(lines[1])[0].endswith("prospero-lld"):
            sys.exit("make_toolchain: clang's driver did not say how it links the broker: %s" % r.stderr[-800:])
        obj = shlex.split(lines[0])[shlex.split(lines[0]).index("-o") + 1]
        link = [{"%s" % obj: "{obj}", os.path.join(tmp, "OUT.elf"): "{out}"}.get(a, a) for a in shlex.split(lines[1])[1:]]
        if "{obj}" not in link or "{out}" not in link:
            sys.exit("make_toolchain: the broker's link names no object or output: %s" % link)
        broker_link = generic(link)
    # (radv-link.sh's recipe, sourced as every title that links RADV does; the builtins named where this folder has them. It
    # writes its version script into the folder: kept as text, written by each link into its own folder)
    script, flags, inputs = toolchain_ps5._recipe(vk, sdk, radv, dict(os.environ, PS5_CLANG=os.path.join(llvm, "clang")))
    clang_builtins = os.path.join(subprocess.run([os.path.join(llvm, "clang"), "--print-resource-dir"], capture_output=True,
                                                 text=True).stdout.strip(), "lib", "linux", "libclang_rt.builtins-x86_64.a")
    mapfile = os.path.join(vk, "build", "radv-platform-local.map")
    with open(mapfile, encoding="utf-8") as f:
        map_text = f.read()

    def link_generic(args):
        return generic([a.replace(clang_builtins, "{builtins}").replace(os.path.realpath(mapfile), "{map}").replace(mapfile, "{map}")
                        for a in args])
    link = {"script": link_generic(script), "flags": link_generic(flags), "inputs": link_generic(inputs), "map": map_text}
    if "{map}" not in link["flags"] or "{builtins}" not in link["inputs"]:
        sys.exit("make_toolchain: radv-link.sh's recipe has no version script or builtins where expected")
    return {"format": 1, "llvm": toolchain_ps5_llvm_version(llvm), "radv": os.path.relpath(radv, out).replace(os.sep, "/"),
            "builtins": os.path.relpath(builtins, out).replace(os.sep, "/"),
            "made_from": "the payload SDK's bin/prospero-clang, prospero-clang++, prospero-lld; PS5_Vulkan's tooling/prospero-clang18 "
                         "and tools/radv-link.sh (all in this folder), run once by make_toolchain.py",
            "compile": compile_, "crt": crt, "lld": lld, "broker_link": broker_link, "link": link}


def toolchain_ps5_llvm_version(llvm):
    return subprocess.run([os.path.join(llvm, "llvm-config"), "--version"], capture_output=True, text=True).stdout.strip()


def native_tool_windows(native, zlib_src, mingw, tool):
    """ps5-native-tool.exe for Windows, cross-built with llvm-mingw: its C++ library and zlib 1.3.2 (PS5_Vulkan's own source
    archive) inside, and a manifest that makes its file names UTF-8 (a player's folder with accents)"""
    import tempfile
    cc, cxx = (os.path.join(mingw, "bin", "x86_64-w64-mingw32-" + n) for n in ("clang", "clang++"))
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for n in ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "zutil.c", "trees.c", "deflate.c", "compress.c",
                  "uncompr.c"):
            o = os.path.join(tmp, n[:-2] + ".o")
            subprocess.run([cc, "-O2", "-w", "-c", os.path.join(zlib_src, n), "-o", o], check=True)
            objs.append(o)
        res = utf8_manifest_resource(mingw, tmp, "ps5-native-tool")
        subprocess.run([cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-static", "-I", zlib_src] +
                       [os.path.join(native, n) for n in NATIVE_SOURCES] + objs + [res, "-o", tool], check=True)


UTF8_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Grapnel.%s" version="1.0.0.0"/>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
"""


def utf8_manifest_resource(mingw, tmp, name, extra_rc=""):
    """A resource object with a manifest that makes a program's ANSI calls UTF-8 (argv, fopen: Windows 10 1903 and later) and
    long-path aware; extra_rc: more resource lines (an icon)"""
    man = os.path.join(tmp, name + ".manifest")
    with open(man, "w", encoding="utf-8") as f:
        f.write(UTF8_MANIFEST % name)
    rc = os.path.join(tmp, name + ".rc")
    with open(rc, "w", encoding="utf-8") as f:
        f.write('#include <winuser.h>\n1 RT_MANIFEST "%s"\n%s' % (man.replace("\\", "/"), extra_rc))
    res = os.path.join(tmp, name + ".res.o")
    subprocess.run([os.path.join(mingw, "bin", "x86_64-w64-mingw32-windres"), rc, "-O", "coff", "-o", res], check=True)
    return res


def main():
    above = toolchain_ps5._above
    ap = argparse.ArgumentParser(description="Grapnel's console toolchain folder for the builder (no game files)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--system", default="linux-x64", choices=("linux-x64", "windows-x64"))
    ap.add_argument("--sdk", default=above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk"))
    ap.add_argument("--sdk-source", default=above("ps5-stack", "PS5_PayloadSDK"), help="the SDK fork's checkout (its LICENSE)")
    ap.add_argument("--ps5-vulkan", default=above("ps5-stack", "PS5_Vulkan"))
    ap.add_argument("--radv-archive", default=above("mesa-work", "game-build", "radv-release-sync", "lib", "libvulkan_radeon.ps5.a"))
    ap.add_argument("--mesa-source", default=above("mesa-work", "game-sync"), help="the RADV branch's checkout (its licence)")
    ap.add_argument("--cpm", default=above("wt", "ps5-port", "third_party-ps5", "cpm"))
    ap.add_argument("--dav1d", default=above("build", "dav1d-ps5"))
    ap.add_argument("--dav1d-include", default=above("third_party", "dav1d", "include"))
    ap.add_argument("--llvm", default="/usr/lib/llvm-19/bin", help="the LLVM 19.1.7 bin folder the native tool is rebuilt with")
    ap.add_argument("--mingw", default=os.path.expanduser("~/.local/opt/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64"),
                    help="llvm-mingw, for --system windows-x64's ps5-native-tool.exe")
    ap.add_argument("--prepare", help="the shaders' preparation for the PC, as make_prepare_tool.py made it (its --out folder)")
    ap.add_argument("--ftpsrv-source", help="a checkout of github.com/ps5-payload-dev/ftpsrv at %s (step 6's FTP server)" % FTPSRV_COMMIT)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    windows = a.system == "windows-x64"
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    # ---- the SDK (its samples and Windows helpers left out) ----
    shutil.copytree(a.sdk, os.path.join(out, "sdk"), symlinks=True, ignore=lambda d, names: [n for n in names if d == a.sdk and
                                                                                               n in ("samples", "win")])
    copy(os.path.join(a.sdk_source, "LICENSE"), os.path.join(out, "sdk", "LICENSE"))
    # ---- PS5_Vulkan's parts ----
    vk = a.ps5_vulkan
    for rel in PS5_VULKAN_FILES:
        copy(os.path.join(vk, rel), os.path.join(out, "ps5_vulkan", rel))
    native = os.path.join(vk, "tooling", "native")
    for n in sorted(os.listdir(native)):
        if os.path.isfile(os.path.join(native, n)):
            copy(os.path.join(native, n), os.path.join(out, "ps5_vulkan", "tooling", "native", n))
    zlib = os.path.join(vk, ".deps", "native", "zlib", "root", "usr")
    os.makedirs(os.path.join(out, "ps5_vulkan", "build", "host"))
    if windows:
        native_tool_windows(native, os.path.join(vk, ".deps", "native", "zlib", "zlib-1.3.2"), a.mingw,
                            os.path.join(out, "ps5_vulkan", "build", "host", "ps5-native-tool.exe"))
    else:
        subprocess.run([os.path.join(a.llvm, "clang++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-static-libstdc++",
                        "-static-libgcc", "-I", os.path.join(zlib, "include")] + [os.path.join(native, n) for n in NATIVE_SOURCES] +
                       [os.path.join(zlib, "lib", "libz.a"), "-o", os.path.join(out, "ps5_vulkan", "build", "host", "ps5-native-tool")],
                       check=True)
    # ---- RADV ----
    # (its debug information names the folders it was built in on the port's PC, 2,080 times: dropped. The eboot never had it:
    # ps5-native-tool keeps the loaded segments only)
    radv = os.path.join(out, "radv", "lib", "libvulkan_radeon.ps5.a")
    copy(a.radv_archive, radv)
    subprocess.run([os.path.join(a.llvm, "llvm-objcopy"), "--strip-debug", radv], check=True)
    prov = os.path.join(os.path.dirname(os.path.dirname(a.radv_archive)), "PROVENANCE.txt")
    with open(prov, encoding="utf-8") as f:   # (its "fork:" line names the folder on the port's PC: written relative)
        text = re.sub(r"/\S*/(mesa-work/\S+)", r"\1", f.read()) + ("toolchain: the archive above with its debug information "
                                                                    "dropped (llvm-objcopy --strip-debug): toolchain.json has its SHA-256\n")
    os.makedirs(os.path.join(out, "radv"), exist_ok=True)
    with open(os.path.join(out, "radv", "PROVENANCE.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    copy(os.path.join(a.mesa_source, "docs", "license.rst"), os.path.join(out, "radv", "license.rst"))
    # ---- the renderer's headers and dav1d ----
    shutil.copytree(a.cpm, os.path.join(out, "cpm"), symlinks=False)
    copy(os.path.join(a.dav1d, "src", "libdav1d.a"), os.path.join(out, "dav1d", "src", "libdav1d.a"))
    shutil.copytree(os.path.join(a.dav1d, "include"), os.path.join(out, "dav1d", "include"))
    shutil.copytree(a.dav1d_include, os.path.join(out, "dav1d", "include-src"))
    copy(os.path.join(os.path.dirname(a.dav1d_include), "COPYING"), os.path.join(out, "dav1d", "COPYING"))
    # ---- (0.3) ftpsrv for the console: the assistant's step 6 ----
    if a.ftpsrv_source:
        build_ftpsrv(a, os.path.join(out, "ftpsrv"))
    # ---- the shaders' preparation for the PC (make_prepare_tool.py made and checked it: no game code, no path of its PC) ----
    if a.prepare:
        prep = json.load(open(os.path.join(a.prepare, "prepare.json")))
        if prep.get("system", "linux-x64") != a.system:   # (one tool per system: make_prepare_tool.py --system)
            sys.exit("make_toolchain: %s is the preparation for %s, not %s" % (a.prepare, prep.get("system", "linux-x64"), a.system))
        for rel, want in prep["files"].items():
            if sha256(os.path.join(a.prepare, *rel.split("/"))) != want["sha256"]:
                sys.exit("make_toolchain: %s/%s is not as its prepare.json says" % (a.prepare, rel))
        shutil.copytree(a.prepare, os.path.join(out, "prepare"), symlinks=False)
    # ---- (0.2) compiler-rt's builtins, and the recipe: the build without bash ----
    clang_res = subprocess.run([os.path.join(a.llvm, "clang"), "--print-resource-dir"], capture_output=True, text=True).stdout.strip()
    builtins = os.path.join(out, "llvm-rt", "libclang_rt.builtins-x86_64.a")
    copy(os.path.join(clang_res, "lib", "linux", "libclang_rt.builtins-x86_64.a"), builtins)
    with open(os.path.join(out, "llvm-rt", "README.md"), "w", encoding="utf-8") as f:
        f.write("libclang_rt.builtins-x86_64.a: compiler-rt's builtins of LLVM %s for x86-64 (an ELF archive), from the LLVM every "
                "tested Grapnel eboot was linked with (Debian's clang 19, upstream 19.1.7). The eboot takes __emutls_get_address "
                "from it (PS5_Vulkan tools/radv-link.sh). Licence: Apache-2.0 WITH LLVM-exception (compiler-rt, part of the LLVM "
                "project: https://github.com/llvm/llvm-project/blob/llvmorg-19.1.7/compiler-rt/LICENSE.TXT).\n"
                % toolchain_ps5_llvm_version(a.llvm))
    recipe = record_recipe(out, a.llvm, builtins, radv)
    with open(os.path.join(out, "ps5_recipe.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(recipe, f, indent=1)
    if windows:
        # (a zip for Windows keeps no symbolic link: the SDK's links to headers and to user/homebrew/include become copies of what
        # they name, at the same place, so the compiles find the same files by the same names)
        for d, dirs, fs in os.walk(out):
            for n in dirs + fs:
                p = os.path.join(d, n)
                if os.path.islink(p):
                    target = os.path.realpath(p)
                    os.remove(p)
                    (shutil.copytree if os.path.isdir(target) else shutil.copy2)(target, p)
        os.remove(os.path.join(out, "ps5_vulkan", "build", "radv-platform-local.map"))   # (direct mode writes its own)
    # ---- nothing of this PC in it: its home folder named in no file (as heydemoura's make_kit checks its link arguments) ----
    home = os.path.expanduser("~").encode()
    user = os.path.basename(os.path.expanduser("~")).encode()
    named = []
    for d, _, fs in os.walk(out):
        for n in fs:
            p = os.path.join(d, n)
            if not os.path.islink(p):
                with open(p, "rb") as f:
                    data = f.read()
                if home in data or (n.endswith((".json", ".md", ".txt", ".map")) and user in data):
                    named.append(os.path.relpath(p, out))
    if named:
        sys.exit("make_toolchain: %d files name this PC's home folder or user, not made: %s" % (len(named), " ".join(named[:20])))
    # ---- what it is ----
    files = {}
    for d, _, fs in os.walk(out):
        for n in fs:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, out).replace(os.sep, "/")
            if rel in GENERATED or os.path.islink(p):
                continue
            files[rel] = {"bytes": os.path.getsize(p), "sha256": sha256(p)}
    radv_rev = next((l.split()[-1] for l in text.splitlines() if l.startswith("revision:")), "")
    info = {"format": 1, "name": "grapnel-toolchain", "version": VERSION, "system": a.system,
            "made": datetime.date.today().isoformat(),
            "llvm": "19.1.7 (the player's own LLVM 19.1.7 compiles and links; this folder has its compiler-rt builtins, llvm-rt/)",
            "recipe": "ps5_recipe.json: the build runs the LLVM tools directly (no bash)",
            "prepare": json.load(open(os.path.join(out, "prepare", "prepare.json")))["shader_stamp"] if a.prepare else None,
            "sources": {"PS5_PayloadSDK": rev(a.sdk_source), "ps5-payload-sdk-installed": open(os.path.join(a.sdk, ".ps5-sdk-revision")).read().strip(),
                        "PS5_Vulkan": rev(vk), "RADV (PS5_Mesa game-radv-align-sync)": radv_rev, "dav1d": rev(os.path.dirname(a.dav1d_include))},
            "files": files}
    with open(os.path.join(out, "toolchain.json"), "w") as f:
        json.dump(info, f, indent=1, sort_keys=True)
    with open(os.path.join(out, "NOTICE.md"), "w") as f:
        f.write("""# Grapnel's console toolchain %(v)s (%(system)s)

What the Grapnel builder needs to make the PS5 title, besides LLVM 19.1.7, Python and the Grapnel repository. **No game file
and nothing made from one is in this folder.** Every part keeps its own licence:

| Folder | What | Licence | Source |
|---|---|---|---|
| `sdk/` | the PS5 payload SDK (ps5-payload-dev/sdk v0.42) with mihawk-99's PS5_PayloadSDK platform layer | GPL-3.0-or-later (`sdk/LICENSE`); its libc++ headers and archives Apache-2.0 WITH LLVM-exception | github.com/ps5-payload-dev/sdk, github.com/mihawk-99/PS5_PayloadSDK at %(sdk)s |
| `ps5_vulkan/` | PS5_Vulkan's link recipe, CRT, AGC link stubs, `runtime/libc.prx` (generated, no Sony code: `runtime/README.md`) and `ps5-native-tool` (%(tool)s) | GPL-3.0-or-later (`ps5_vulkan/LICENSE`, `NOTICE.md`) | github.com/mihawk-99/PS5_Vulkan at %(vk)s |
| `radv/` | Mesa's RADV for the PS5, the Grapnel branch of PS5_Mesa (`radv/PROVENANCE.txt`) | MIT (`radv/license.rst`) | PS5_Mesa %(radv)s (the branch is to be published with Grapnel) |
| `cpm/` | Vulkan-Headers v1.4.345, SPIRV-Headers vulkan-sdk-1.4.341.0, VulkanMemoryAllocator v3.3.0 | Apache-2.0 / MIT, their LICENSE files | github.com/KhronosGroup, github.com/GPUOpen-LibrariesAndSDKs |
| `dav1d/` | dav1d 1.5.1 built for the console | BSD-2-Clause (`dav1d/COPYING`) | code.videolan.org/videolan/dav1d at %(dav1d)s |
| `llvm-rt/` | compiler-rt's builtins of LLVM 19.1.7 for x86-64 (`llvm-rt/README.md`) | Apache-2.0 WITH LLVM-exception | github.com/llvm/llvm-project, llvmorg-19.1.7 |
| `ps5_recipe.json` | the arguments `sdk/bin/prospero-*`, `ps5_vulkan/tooling/prospero-clang18` and `ps5_vulkan/tools/radv-link.sh` give the LLVM tools, recorded from those scripts | as those scripts: GPL-3.0-or-later | made by Grapnel's `scripts/grapnel-builder/make_toolchain.py` |
%(ftpsrv)s%(prepare)s
""" % {"ftpsrv": "" if not a.ftpsrv_source else
           "| `ftpsrv/` | `ftpsrv-ps5.elf`, the FTP server the builder's step 6 sends to the console's ELF loader, compiled here from `ftpsrv/src/` | GPL-3.0-or-later (`ftpsrv/LICENSE`); sha256.c public domain | github.com/ps5-payload-dev/ftpsrv at %s (John Törnblom) |\n" % FTPSRV_COMMIT,
       "prepare": "" if not a.prepare else
           "| `prepare/` | the shaders' preparation for the PC (`prepare/prepare.json`): Grapnel's Windows host and renderer (no game code, LLVM's libc++ and libunwind inside), the PS5 RADV built for Windows on its winsys's host model (`vulkan_radeon.dll`, MinGW-w64's runtime and winpthreads inside) | GPL-3.0-or-later; MIT; Apache-2.0 WITH LLVM-exception; MinGW-w64's (`prepare/LICENSES/`) | this repository; PS5_Mesa; llvm-mingw 20250114 |"
           if windows else
           "| `prepare/` | the shaders' preparation for the PC (`prepare/prepare.json`): Grapnel's Linux host and renderer (no game code), the PS5 RADV built for Linux on its winsys's host model, libdrm, LLVM's libc++/libc++abi/libunwind, the Khronos Vulkan loader | GPL-3.0-or-later; MIT; MIT; Apache-2.0 WITH LLVM-exception; Apache-2.0 (`prepare/LICENSES/`) | this repository; PS5_Mesa; Debian's packages |",
       "v": VERSION, "system": a.system, "sdk": info["sources"]["PS5_PayloadSDK"][:12], "vk": info["sources"]["PS5_Vulkan"][:12],
           "radv": radv_rev[:12], "dav1d": info["sources"]["dav1d"][:12],
           "tool": "ps5-native-tool.exe cross-built for Windows with llvm-mingw, with zlib 1.3.2 inside, zlib licence" if windows else
                   "rebuilt here with zlib 1.3.2, zlib licence"})
    total = sum(v["bytes"] for v in files.values())
    print("toolchain: %s, %d files, %.0f MB" % (out, len(files), total / 1e6))


if __name__ == "__main__":
    main()
