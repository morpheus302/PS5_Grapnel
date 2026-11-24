#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A check with no game: the Windows builder compiles the port byte for byte as Linux does. Run before a package is sent.

    python3 -I scripts/grapnel-builder/check_windows_objects.py --package <stage>/Grapnel --wineprefix <prefix>
        --linux-toolchain <grapnel-toolchain-*-linux-x64> --work <folder> [--linux-llvm D] [--jobs N]

The renderer alone (nx2/build.py with RX_VK_ONLY=1 and no link: gpu/vk, Eden's recompiler, sirit, Dear ImGui, MihawkGen,
zstd; ~215 objects, no game file needed) is built twice from the package's own app folder: on Linux with LLVM 19.1.7 and the
Linux toolchain, and under Wine with the package's python, llvm and toolchain folders. Every object is compared without its
.comment section (the compiler's name, which the signed eboot does not keep). Exit 0 when all are the same.

Why (23/11/2026): the first Windows package's LLVM (llvm-mingw's, built on libc++) compiled one function of
gpu/vk/vk_texture.cpp otherwise than every libstdc++-built clang 19.1.7, and the player's eboot differed from Linux's. This
check finds it in five minutes, with no game. Wine needs Microsoft's ucrtbase.dll in its prefix (Wine 10's lacks crealf,
which numpy imports; the renderer's build does not need numpy, so the check runs without it too).
"""
import argparse
import os
import subprocess
import sys
import tempfile


DRIVER = r'''
import os, subprocess, sys
app, work, tc, llvm, jobs = sys.argv[1:6]
sys.path.insert(0, os.path.join(app, "scripts", "grapnel-builder"))
import grapnel_core as core
env = core.tools_env(core.tools(tc), core.find_llvm(llvm, need_builtins=False), work)
env.update(OOT_WORK=work, RX_VK_ONLY="1", HOOKSHOT_PS5_NO_LINK="1", HOOKSHOT_PS5_FLEXIBLE_MB=str(core.FLEXIBLE_MB))
os.makedirs(os.path.join(work, "sa", "gen"), exist_ok=True)
open(os.path.join(work, "sa", "gen", "a64_stubs.c"), "w").close()   # (ps5_stubs reads the lift's stubs: none here, no game)
with open(os.path.join(work, "renderer.log"), "w") as log:
    r = subprocess.run([sys.executable, "-I", "-B", "-u", os.path.join(app, "nx2", "build.py"), "--standalone", "--jobs", jobs],
                       env=env, cwd=app, stdout=log, stderr=subprocess.STDOUT)
sys.exit(r.returncode)
'''


def windows_path(p):
    return "Z:" + p.replace("/", "\\")


def objects(work):
    d = os.path.join(work, "sa", "build-ps5", "obj")
    return sorted(f for f in os.listdir(d) if f.endswith(".o")) if os.path.isdir(d) else []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--package", required=True, help="the Windows builder's folder (Grapnel, with app, python, llvm, toolchain)")
    ap.add_argument("--wineprefix", required=True)
    ap.add_argument("--linux-toolchain", required=True, help="the Linux toolchain folder of the same version as the package's")
    ap.add_argument("--linux-llvm", default="/usr/lib/llvm-19/bin")
    ap.add_argument("--work", required=True, help="a folder for the two builds (deleted after), on a disk with 1 GB free")
    ap.add_argument("--jobs", default="8")
    a = ap.parse_args()
    pkg, work = os.path.abspath(a.package), os.path.abspath(a.work)
    app = os.path.join(pkg, "app")
    import shutil
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    driver = os.path.join(work, "driver.py")
    with open(driver, "w") as f:
        f.write(DRIVER)
    lin, win = os.path.join(work, "linux"), os.path.join(work, "windows")
    print("check: the renderer on Linux", flush=True)
    r = subprocess.run([sys.executable, "-I", driver, app, lin, a.linux_toolchain, a.linux_llvm, a.jobs])
    if r.returncode:
        sys.exit("check: the Linux build failed: %s" % os.path.join(lin, "renderer.log"))
    print("check: the renderer under Wine, with the package's tools", flush=True)
    env = dict(os.environ, WINEPREFIX=a.wineprefix, WINEDLLOVERRIDES="mscoree,mshtml=;ucrtbase=n,b", WINEDEBUG="-all")
    r = subprocess.run(["wine", os.path.join(pkg, "python", "python.exe"), "-I", windows_path(driver), windows_path(app),
                        windows_path(win), windows_path(os.path.join(pkg, "toolchain")), windows_path(os.path.join(pkg, "llvm")),
                        a.jobs], env=env)
    if r.returncode:
        sys.exit("check: the Windows build failed: %s" % os.path.join(win, "renderer.log"))
    names = objects(lin)
    if not names or names != objects(win):
        sys.exit("check: the two builds made different sets of objects (%d, %d)" % (len(names), len(objects(win))))
    objcopy = os.path.join(a.linux_llvm, "llvm-objcopy")
    differ = []
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            data = []
            for side in (lin, win):
                out = os.path.join(tmp, "o")
                subprocess.run([objcopy, "--remove-section=.comment", os.path.join(side, "sa", "build-ps5", "obj", n), out], check=True)
                with open(out, "rb") as f:
                    data.append(f.read())
            if data[0] != data[1]:
                differ.append(n)
    print("check: %d objects, %d differ%s" % (len(names), len(differ), (": " + " ".join(differ)) if differ else ""))
    shutil.rmtree(work, ignore_errors=True)
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
