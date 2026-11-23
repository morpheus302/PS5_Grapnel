#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel's console toolchain for the builder, laid out in one folder from a developer's PC: everything the build of the PS5
title needs that is neither the game nor in this repository, and no game file at all.

    python3 -I scripts/grapnel-builder/make_toolchain.py --out <folder> [--sdk D] [--ps5-vulkan D] [--radv-archive F]
        [--cpm D] [--dav1d D] [--dav1d-include D] [--llvm D] [--prepare D]

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
    <folder>/prepare/         (0.2) the shaders' preparation for the PC, made by make_prepare_tool.py (--prepare: its folder):
                              the console tree's Linux host and this release's renderer with no game code, the console driver's
                              PC model and their libraries (the builder's step 7 runs it on the player's PC)

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

VERSION = "0.2"   # (0.2: prepare/, the shaders' preparation for the PC)
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


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def main():
    above = toolchain_ps5._above
    ap = argparse.ArgumentParser(description="Grapnel's console toolchain folder for the builder (no game files)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sdk", default=above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk"))
    ap.add_argument("--sdk-source", default=above("ps5-stack", "PS5_PayloadSDK"), help="the SDK fork's checkout (its LICENSE)")
    ap.add_argument("--ps5-vulkan", default=above("ps5-stack", "PS5_Vulkan"))
    ap.add_argument("--radv-archive", default=above("mesa-work", "game-build", "radv-release-sync", "lib", "libvulkan_radeon.ps5.a"))
    ap.add_argument("--mesa-source", default=above("mesa-work", "game-sync"), help="the RADV branch's checkout (its licence)")
    ap.add_argument("--cpm", default=above("wt", "ps5-port", "third_party-ps5", "cpm"))
    ap.add_argument("--dav1d", default=above("build", "dav1d-ps5"))
    ap.add_argument("--dav1d-include", default=above("third_party", "dav1d", "include"))
    ap.add_argument("--llvm", default="/usr/lib/llvm-19/bin", help="the LLVM 19.1.7 bin folder the native tool is rebuilt with")
    ap.add_argument("--prepare", help="the shaders' preparation for the PC, as make_prepare_tool.py made it (its --out folder)")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
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
    tool = os.path.join(out, "ps5_vulkan", "build", "host", "ps5-native-tool")
    os.makedirs(os.path.dirname(tool))
    subprocess.run([os.path.join(a.llvm, "clang++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-static-libstdc++",
                    "-static-libgcc", "-I", os.path.join(zlib, "include")] + [os.path.join(native, n) for n in NATIVE_SOURCES] +
                   [os.path.join(zlib, "lib", "libz.a"), "-o", tool], check=True)
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
    # ---- the shaders' preparation for the PC (make_prepare_tool.py made and checked it: no game code, no path of its PC) ----
    if a.prepare:
        prep = json.load(open(os.path.join(a.prepare, "prepare.json")))
        for rel, want in prep["files"].items():
            if sha256(os.path.join(a.prepare, *rel.split("/"))) != want["sha256"]:
                sys.exit("make_toolchain: %s/%s is not as its prepare.json says" % (a.prepare, rel))
        shutil.copytree(a.prepare, os.path.join(out, "prepare"), symlinks=False)
    # ---- nothing of this PC in it: its home folder named in no file (as heydemoura's make_kit checks its link arguments) ----
    home = os.path.expanduser("~").encode()
    named = []
    for d, _, fs in os.walk(out):
        for n in fs:
            p = os.path.join(d, n)
            if not os.path.islink(p):
                with open(p, "rb") as f:
                    if home in f.read():
                        named.append(os.path.relpath(p, out))
    if named:
        sys.exit("make_toolchain: %d files name this PC's home folder, not made: %s" % (len(named), " ".join(named[:20])))
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
    info = {"format": 1, "name": "grapnel-toolchain", "version": VERSION, "system": "linux-x64",
            "made": datetime.date.today().isoformat(),
            "llvm": "19.1.7 (the player's own LLVM 19.1.7 compiles and links; this folder has none of LLVM)",
            "prepare": json.load(open(os.path.join(out, "prepare", "prepare.json")))["shader_stamp"] if a.prepare else None,
            "sources": {"PS5_PayloadSDK": rev(a.sdk_source), "ps5-payload-sdk-installed": open(os.path.join(a.sdk, ".ps5-sdk-revision")).read().strip(),
                        "PS5_Vulkan": rev(vk), "RADV (PS5_Mesa game-radv-align-sync)": radv_rev, "dav1d": rev(os.path.dirname(a.dav1d_include))},
            "files": files}
    with open(os.path.join(out, "toolchain.json"), "w") as f:
        json.dump(info, f, indent=1, sort_keys=True)
    with open(os.path.join(out, "NOTICE.md"), "w") as f:
        f.write("""# Grapnel's console toolchain %(v)s (linux-x64)

What the Grapnel builder needs to make the PS5 title, besides LLVM 19.1.7, Python and the Grapnel repository. **No game file
and nothing made from one is in this folder.** Every part keeps its own licence:

| Folder | What | Licence | Source |
|---|---|---|---|
| `sdk/` | the PS5 payload SDK (ps5-payload-dev/sdk v0.42) with mihawk-99's PS5_PayloadSDK platform layer | GPL-3.0-or-later (`sdk/LICENSE`); its libc++ headers and archives Apache-2.0 WITH LLVM-exception | github.com/ps5-payload-dev/sdk, github.com/mihawk-99/PS5_PayloadSDK at %(sdk)s |
| `ps5_vulkan/` | PS5_Vulkan's link recipe, CRT, AGC link stubs, `runtime/libc.prx` (generated, no Sony code: `runtime/README.md`) and `ps5-native-tool` (rebuilt here with zlib 1.3.2, zlib licence) | GPL-3.0-or-later (`ps5_vulkan/LICENSE`, `NOTICE.md`) | github.com/mihawk-99/PS5_Vulkan at %(vk)s |
| `radv/` | Mesa's RADV for the PS5, the Grapnel branch of PS5_Mesa (`radv/PROVENANCE.txt`) | MIT (`radv/license.rst`) | PS5_Mesa %(radv)s (the branch is to be published with Grapnel) |
| `cpm/` | Vulkan-Headers v1.4.345, SPIRV-Headers vulkan-sdk-1.4.341.0, VulkanMemoryAllocator v3.3.0 | Apache-2.0 / MIT, their LICENSE files | github.com/KhronosGroup, github.com/GPUOpen-LibrariesAndSDKs |
| `dav1d/` | dav1d 1.5.1 built for the console | BSD-2-Clause (`dav1d/COPYING`) | code.videolan.org/videolan/dav1d at %(dav1d)s |
| `prepare/` | the shaders' preparation for the PC (`prepare/prepare.json`): Grapnel's Linux host and renderer (no game code), the PS5 RADV built for Linux on its winsys's host model, libdrm, LLVM's libc++/libc++abi/libunwind, the Khronos Vulkan loader | GPL-3.0-or-later; MIT; MIT; Apache-2.0 WITH LLVM-exception; Apache-2.0 (`prepare/LICENSES/`) | this repository; PS5_Mesa; Debian's packages |
""" % {"v": VERSION, "sdk": info["sources"]["PS5_PayloadSDK"][:12], "vk": info["sources"]["PS5_Vulkan"][:12],
           "radv": radv_rev[:12], "dav1d": info["sources"]["dav1d"][:12]})
    total = sum(v["bytes"] for v in files.values())
    print("toolchain: %s, %d files, %.0f MB" % (out, len(files), total / 1e6))


if __name__ == "__main__":
    main()
