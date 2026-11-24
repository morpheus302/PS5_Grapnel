#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Grapnel builder for Windows, made on Linux: a portable folder in a zip, nothing to install, no administrator.

    python3 -I scripts/grapnel-builder/make_windows_package.py [--version 0.2-test1] [--out D] [--toolchain D]

    <out>/Grapnel-Builder-<version>-windows-x64.zip, its one folder Grapnel\\:
      Grapnel.exe            the start: the window (window/launcher.c, with the brass icon)
      Grapnel-console.cmd    the same build without the window, in a console (grapnel_build.py's options)
      LISEZMOI.txt, README.txt, NOTICE.txt, LICENSES\\
      app\\                   this repository at HEAD (git archive, without the web site and CI folders), VERSION (git describe),
                             and its two helper programs built for Windows: nx2\\zbic_unpack\\zbic_unpack.exe (GPL-2.0, a program of
                             its own) and third_party\\a64-kit\\lift\\a64emit.dll (the translator's suyu part)
      python\\                CPython 3.13 (python-build-standalone, with Tkinter) and the pinned wheels of requirements.txt
      llvm\\                  LLVM 19.1.7 for Windows as WinLibs builds it (GCC 14.2 and libstdc++): clang, ld.lld, llvm-ar,
                             llvm-nm, the DLLs they load, clang's headers, LICENSES\\. Two things decide which Windows LLVM:
                             - LLVM's code depends on the C++ library LLVM itself was built with: llvm-mingw's (libc++) compiled
                               gpu/vk/vk_texture.cpp's texture_view otherwise than every libstdc++-built clang 19.1.7 (Debian's,
                               the official Linux one, WinLibs' for Windows), and the player's eboot differed from Linux's
                               (23/11/2026). Built on libstdc++ as Linux's is, WinLibs' makes the renderer's 215 objects byte for
                               byte. The package is refused if its LLVM loads libc++;
                             - a MinGW-built LLVM puts "/" between a folder and a header, where the official MSVC-built one puts
                               "\\": with it and nx2/toolchain_ps5.py forward_slashes every __FILE__ string is Linux's
      toolchain\\             the console toolchain, make_toolchain.py --system windows-x64 (ps5_recipe.json: no bash)

Every download is checked against the SHA-256 named here. The folder is refused if a file names this PC (its home folder or
user name) or a private network address, or looks like a game file. heydemoura's Longshot PS5 Builder (ps5/builder/package.py,
GPL-3.0) packs its Windows zip on Linux the same way; this is written for Grapnel's own layout.
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PS5 = os.path.dirname(os.path.dirname(ROOT))   # (hookshot-ps5, above wt/<worktree>)

DOWNLOADS = {   # name: (url, sha256)
    "winlibs": ("https://github.com/brechtsanders/winlibs_mingw/releases/download/14.2.0posix-19.1.7-12.0.0-msvcrt-r3/"
                "winlibs-x86_64-posix-seh-gcc-14.2.0-llvm-19.1.7-mingw-w64msvcrt-12.0.0-r3.7z",
                "4a0fd9b9621c3a82cc4f2700cc07c73fc056da50ff147d63828f2899ca3d7d8e"),   # (the release's own .sha256)
    "python": ("https://github.com/astral-sh/python-build-standalone/releases/download/20261110/"
               "cpython-3.13.18%2B20261110-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
               "2e0fed09d48989f64c9305da9ff2d37a41c0d13ab2aa6283405b2b620dfbf805"),
}
WHEELS = {   # requirements.txt's pins, for CPython 3.13 on Windows x64 (pip download --platform win_amd64)
    "archspec-0.2.7-py3-none-any.whl": "b6d97c2d840d8538d3f63b70c7c45ace2d26b3d9591d7c93d5fb33e50b1bc592",
    "astc_encoder_py-0.1.12-cp37-abi3-win_amd64.whl": "a45d375fc6b0ff2cd3d772aab3eef19a6b7f3e067306e11fab37820ea9576121",
    "capstone-5.0.9-py3-none-win_amd64.whl": "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0",
    "fonttools-4.68.0-cp313-cp313-win_amd64.whl": "6ed796b220654b4d37ad20af73419ec7969b669dd678cab88afb602c5683a331",
    "lz4-4.4.5-cp313-cp313-win_amd64.whl": "da68497f78953017deb20edff0dba95641cc86e7423dfadf7c0264e1ac60dc22",
    "numpy-2.5.4-cp313-cp313-win_amd64.whl": "8b4d2fd2d34e5f8c9235ee787de5631a37a28402b15cb80814df973d2be54129",
    "pillow-13.0.0-cp313-cp313-win_amd64.whl": "9dda0788a9932776a75d8bcc020076edc308ce2508b51f6739a45e3158b3fc42",
    "texture2ddecoder-1.0.6-cp311-abi3-win_amd64.whl": "1bad8209d9dfbfc7daf13ea2e545236ea2bbc1e423b2992b6e7563909975a47b",
    "zstandard-0.25.0-cp313-cp313-win_amd64.whl": "1f830a0dac88719af0ae43b8b2d6aef487d437036468ef3c2ea59c51f9d55fd5",
}
LLVM_TOOLS = ("clang.exe", "ld.lld.exe", "llvm-ar.exe", "llvm-nm.exe", "llvm-objcopy.exe")   # (objcopy: the broker's .comment out)
LICENSE_TEXTS = {   # (the DLLs' licences: WinLibs' archive has none) name: (url, sha256)
    "LLVM-LICENSE.TXT": ("https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-19.1.7/llvm/LICENSE.TXT",
                         "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"),
    "GCC-COPYING.RUNTIME": ("https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-14.2.0/COPYING.RUNTIME",
                            "9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74"),
    "winpthreads-COPYING": ("https://raw.githubusercontent.com/mingw-w64/mingw-w64/v12.0.0/mingw-w64-libraries/winpthreads/COPYING",
                            "63263614cdd29f2f93cba85e992f041b31f9fc7b4033692f31269489a8a1b177"),
    "zlib-LICENSE": ("https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE",
                     "845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243"),
    "zstd-LICENSE": ("https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE",
                     "7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8"),
    "libxml2-Copyright": ("https://raw.githubusercontent.com/GNOME/libxml2/v2.13.5/Copyright",
                          "c99aae1afe013e50b8b3701e089222b351258043c3025b64053a233fd25b4be7"),
    "LGPL-2.1.txt": ("https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt",
                     "20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95"),
    "LGPL-3.0.txt": ("https://www.gnu.org/licenses/lgpl-3.0.txt", "e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118"),
    "z3-LICENSE.txt": ("https://raw.githubusercontent.com/Z3Prover/z3/z3-4.13.4/LICENSE.txt",
                       "e617cad2ab9347e3129c2b171e87909332174e17961c5c3412d0799469111337"),
}
LLVM_README = """LLVM 19.1.7 for Windows, as WinLibs 14.2.0posix-19.1.7-12.0.0-msvcrt-r3 builds it (github.com/brechtsanders/winlibs_mingw:
LLVM built with GCC 14.2 and its libstdc++, against MSVCRT). Only what the builder runs: clang.exe, ld.lld.exe, llvm-ar.exe,
llvm-nm.exe, llvm-objcopy.exe, the DLLs they load, and clang's own headers (lib/clang/19/include).

Why this LLVM: LLVM's output depends on the C++ library LLVM itself was built with. Every clang 19.1.7 built on libstdc++
(Debian's, LLVM's own Linux release, this one) compiles the port to the same bytes; llvm-mingw's, built on libc++, compiled
one function of the renderer otherwise, and the eboot made on Windows differed from Linux's.

Licences (LICENSES/): LLVM, clang, lld - Apache-2.0 WITH LLVM-exception (LLVM-LICENSE.TXT); libstdc++-6.dll,
libgcc_s_seh-1.dll - GPL-3.0 with the GCC Runtime Library Exception 3.1 (GCC-COPYING.RUNTIME, and ../../LICENSES/GPL-3.0.txt);
libwinpthread-1.dll - mingw-w64's MIT/BSD terms (winpthreads-COPYING); zlib1.dll - zlib (zlib-LICENSE); libzstd.dll -
BSD-3-Clause (zstd-LICENSE); libxml2-2.dll - MIT (libxml2-Copyright); libiconv-2.dll - LGPL-2.1 (LGPL-2.1.txt); libgmp-10.dll -
LGPL-3.0 or GPL-2.0 (LGPL-3.0.txt, ../../LICENSES/GPL-2.0.txt); libz3.dll - MIT (z3-LICENSE.txt). The LGPL libraries are
separate DLLs, replaceable by the player; their sources: gnu.org/software/libiconv, gmplib.org, and WinLibs' build scripts.
"""
LEFT_OUT = ("website/", ".forgejo/", ".circleci/", ".claude/", "tests/")   # (the web site, CI and checks: not the builder's)
GAME_NAMES = re.compile(r"(^|/)(eboot\.bin|image\.bin|main|main\.npdm|rtld|sdk|subsdk\d+|.*\.nup|.*\.nso|.*\.nca|.*\.xci|.*\.nsp|"
                        r"code_(?:hot_)?\d[\d_]*\.c|code_hot\.c\.whole|a64_table\.c|hookshot_shaders\.bin|longshot_shaders\.bin)$",
                        re.I)
PRIVATE_NET = re.compile(rb"(?<![\d.])(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}(?![\d.])")
OURS = ("app/scripts/", "app/nx2/", "app/host/", "app/gpu/vk/vk_", "app/assets/", "Grapnel", "LISEZMOI", "README", "NOTICE",
        "toolchain/ps5_recipe.json", "toolchain/toolchain.json", "toolchain/NOTICE.md")   # (where a private address would be ours)
TEXT_EXT = (".py", ".txt", ".md", ".json", ".cmd", ".cfg", ".ini", ".toml", ".c", ".h", ".cpp", ".rc", ".manifest", ".map", ".ld")


def say(text):
    print("package: " + text, flush=True)


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


def fetch(name, cache, path=None):
    url, want = DOWNLOADS[name]
    folder = os.path.join(cache, name)
    path = path or os.path.join(folder, url.rsplit("/", 1)[1].replace("%2B", "+"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.exists(path):
        say("downloading " + url)
        urllib.request.urlretrieve(url, path + ".part")
        os.replace(path + ".part", path)
    got = sha256(path)
    if got != want:
        sys.exit("package: %s has SHA-256 %s, not the %s named here: not used" % (path, got, want))
    return path


def wheels(cache, python):
    folder = os.path.join(cache, "wheels")
    missing = [w for w in WHEELS if not os.path.exists(os.path.join(folder, w))]
    if missing:
        os.makedirs(folder, exist_ok=True)
        pins = [l.strip() for l in open(os.path.join(HERE, "requirements.txt")) if l.strip() and not l.startswith("#")]
        say("downloading the wheels: " + " ".join(pins))
        subprocess.run([python, "-m", "pip", "download", "-q", "--disable-pip-version-check", "--only-binary=:all:", "--platform",
                        "win_amd64", "--python-version", "3.13", "--implementation", "cp", "-d", folder] + pins, check=True)
    out = []
    for w, want in sorted(WHEELS.items()):
        p = os.path.join(folder, w)
        if sha256(p) != want:
            sys.exit("package: %s is not the wheel pinned here" % p)
        out.append(p)
    return out


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True).stdout.strip()


def stage_app(app, mingw, allow_dirty):
    """The repository at HEAD, its VERSION, and its two helper programs built for Windows"""
    if git("status", "--porcelain", "--untracked-files=no") and not allow_dirty:
        sys.exit("package: the checkout has changes: commit them (VERSION names HEAD, and the eboot's version string with it)")
    data = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", "HEAD"], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        members = [m for m in t.getmembers() if not m.name.startswith(LEFT_OUT) and not m.issym() and not m.islnk()]
        t.extractall(app, members=members, filter="data")
    version = git("describe", "--tags", "--always", "--match", "v[0-9]*")
    with open(os.path.join(app, "VERSION"), "w", newline="\n") as f:
        f.write(version + "\n")
    cc, cxx = (os.path.join(mingw, "bin", "x86_64-w64-mingw32-" + n) for n in ("clang", "clang++"))
    lift = os.path.join(app, "third_party", "a64-kit", "lift")
    # (-ffile-prefix-map: an assert's __FILE__ named the stage folder on this PC, its user name with it)
    run([cxx, "-std=c++20", "-O2", "-shared", "-static", "-Wno-everything", "-ffile-prefix-map=%s=." % app, "-o", os.path.join(lift, "a64emit.dll"),
         os.path.join(lift, "a64emit.cpp")])
    key = hashlib.sha1(b"".join(open(os.path.join(lift, f), "rb").read() for f in ("a64emit.cpp", "suyu/arm64_to_c.h"))).hexdigest()
    with open(os.path.join(lift, "a64emit.dll.sha1"), "w") as f:   # (build_emit.py's stamp: the library is taken as built)
        f.write(key)
    zbic = os.path.join(app, "third_party", "zbic")
    run([cc, "-O2", "-static", "-DZSTD_ZBIC_SUPPORT=1", "-ffile-prefix-map=%s=." % app, "-I", zbic, "-o",
         os.path.join(app, "nx2", "zbic_unpack", "zbic_unpack.exe"),
         os.path.join(app, "nx2", "zbic_unpack", "zbic_unpack.c"), os.path.join(zbic, "zstd.c")])
    return version


def stage_python(top, cache):
    with tarfile.open(fetch("python", cache)) as t:
        t.extractall(top, filter="data")   # (its one folder: python/)
    site = os.path.join(top, "python", "Lib", "site-packages")
    for w in wheels(cache, sys.executable if os.path.exists(os.path.join(os.path.dirname(sys.executable), "pip")) else
                    os.path.join(PS5, ".venv", "bin", "python3")):
        with zipfile.ZipFile(w) as z:
            z.extractall(site)
    shutil.rmtree(os.path.join(top, "python", "Lib", "test"), ignore_errors=True)


def pe_imports(path, readobj):
    out = subprocess.run([readobj, "--coff-imports", path], capture_output=True, text=True, check=True).stdout
    return [l.split()[1] for l in out.splitlines() if l.strip().startswith("Name:")]


def stage_llvm(top, cache, readobj):
    """WinLibs' LLVM: the four tools, every DLL of the archive they load (their imports, followed), clang's headers, the licences"""
    archive = fetch("winlibs", cache)
    unpacked = os.path.join(cache, "winlibs", "unpacked")
    if not all(os.path.isfile(os.path.join(unpacked, "mingw64", "bin", t)) for t in LLVM_TOOLS):
        shutil.rmtree(unpacked, ignore_errors=True)
        run(["7z", "x", "-y", "-o" + unpacked, archive, "mingw64/bin/*.dll", "mingw64/lib/clang/19/include/*"] +
            ["mingw64/bin/" + t for t in LLVM_TOOLS], stdout=subprocess.DEVNULL)
    b = os.path.join(unpacked, "mingw64", "bin")
    have = {n.lower(): n for n in os.listdir(b)}
    need, todo = {}, list(LLVM_TOOLS)
    while todo:
        n = todo.pop()
        if n.lower() in need:
            continue
        need[n.lower()] = have[n.lower()]
        todo += [d for d in pe_imports(os.path.join(b, have[n.lower()]), readobj) if d.lower() in have]
    if "libc++.dll" in need or "libstdc++-6.dll" not in need:
        sys.exit("package: this LLVM is not built on libstdc++ (as Linux's is): its code would differ from Linux's")
    os.makedirs(os.path.join(top, "llvm", "bin"))
    for n in sorted(need.values()):
        shutil.copy2(os.path.join(b, n), os.path.join(top, "llvm", "bin", n))
    shutil.copytree(os.path.join(unpacked, "mingw64", "lib", "clang", "19", "include"),
                    os.path.join(top, "llvm", "lib", "clang", "19", "include"))
    lic = os.path.join(top, "llvm", "LICENSES")
    os.makedirs(lic)
    for name, (url, want) in LICENSE_TEXTS.items():
        DOWNLOADS["licence " + name] = (url, want)
        shutil.copyfile(fetch("licence " + name, cache, os.path.join(cache, "licenses", name)), os.path.join(lic, name))
    with open(os.path.join(top, "llvm", "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(LLVM_README)
    say("llvm: %d files (%s)" % (len(need), ", ".join(sorted(n for n in need.values() if not n.lower().startswith("libllvm")
                                                             and not n.lower().startswith("libclang")))))


# (0.3-test3) Windows' own DLLs, the only ones a static LLVM may load
WINDOWS_DLLS = {"kernel32.dll", "advapi32.dll", "msvcrt.dll", "ntdll.dll", "ole32.dll", "shell32.dll", "version.dll", "user32.dll",
                "ws2_32.dll", "bcrypt.dll", "psapi.dll", "dbghelp.dll"}
LLVM_BUILT_README = """LLVM 19.1.7 for Windows, built for Grapnel (scripts/grapnel-builder/make_windows_llvm.sh): clang, lld and the
binutils the builder runs, compiled by clang 19.1.7 (llvm-mingw 20250114) against WinLibs' GCC 14.2 runtime (its libstdc++,
MSVCRT, winpthreads), everything static: clang.exe, ld.lld.exe, llvm-ar.exe, llvm-nm.exe and llvm-objcopy.exe load no DLL but
Windows' own; clang's headers in lib/clang/19/include.

Why this LLVM: LLVM's output depends on the C++ library LLVM itself was built with. Every clang 19.1.7 built on libstdc++
(Debian's, LLVM's own Linux release, WinLibs', this one) compiles the port to the same bytes; llvm-mingw's, built on libc++,
compiled one function of the renderer otherwise. WinLibs' own build (the earlier test packages') is split in about 150 DLLs
built by GCC, and compiled the lifted code 25-30 %% slower than llvm-mingw's on a player's PC; built by clang and static, this
one is the fastest of the three under the same test (notes: notes-agents/constructeur-windows.md).

Licences (LICENSES/): LLVM, clang, lld - Apache-2.0 WITH LLVM-exception (LLVM-LICENSE.TXT); libstdc++ and libgcc, linked in -
GPL-3.0 with the GCC Runtime Library Exception 3.1 (GCC-COPYING.RUNTIME, and ../../LICENSES/GPL-3.0.txt); winpthreads and
MinGW-w64's runtime, linked in - mingw-w64's MIT/BSD terms (winpthreads-COPYING). Sources: github.com/llvm/llvm-project at
llvmorg-19.1.7; GCC 14.2 and mingw-w64 12 as WinLibs builds them (github.com/brechtsanders/winlibs_mingw).
"""


LLVM_NOTICE_WINLIBS = """    LLVM 19.1.7 for Windows as WinLibs builds it (github.com/brechtsanders/winlibs_mingw, 14.2.0posix-19.1.7-12.0.0-msvcrt-r3:
    built with GCC 14.2 and libstdc++): clang, ld.lld, llvm-ar, llvm-nm, the DLLs they load, clang's headers. LLVM:
    Apache-2.0 WITH LLVM-exception; the GCC runtime DLLs: GPL-3.0 with the GCC Runtime Library Exception; winpthreads,
    zlib, zstd, libxml2, Z3: permissive; libiconv (LGPL-2.1) and GMP (LGPL-3.0 or GPL-2.0) as separate, replaceable DLLs.
    Each text in llvm\\LICENSES, the list in llvm\\README.txt."""
LLVM_NOTICE_BUILT = """    LLVM 19.1.7 for Windows built for Grapnel (scripts\\grapnel-builder\\make_windows_llvm.sh): clang, ld.lld, llvm-ar, llvm-nm,
    llvm-objcopy, static, compiled by clang 19.1.7 against WinLibs' GCC 14.2 runtime (libstdc++, MSVCRT, winpthreads, linked
    in); clang's headers. LLVM: Apache-2.0 WITH LLVM-exception; libstdc++ and libgcc: GPL-3.0 with the GCC Runtime Library
    Exception; winpthreads and MinGW-w64's runtime: their MIT/BSD terms. Each text in llvm\\LICENSES, the rest in
    llvm\\README.txt."""


def stage_llvm_built(top, build, cache, readobj):
    """(0.3-test3) The static LLVM built by make_windows_llvm.sh: its five tools, which may load Windows' own DLLs only, clang's
    headers, the licences"""
    b = os.path.join(build, "bin")
    os.makedirs(os.path.join(top, "llvm", "bin"))
    for n in LLVM_TOOLS:
        dlls = {d.lower() for d in pe_imports(os.path.join(b, n), readobj)}
        other = sorted(d for d in dlls - WINDOWS_DLLS if not d.startswith("api-ms-win-"))
        if other:
            sys.exit("package: %s loads %s: not the static LLVM" % (n, ", ".join(other)))
        out = subprocess.run([readobj, "--coff-imports", os.path.join(b, n)], capture_output=True, text=True).stdout
        if "libc++" in out:
            sys.exit("package: %s is built on libc++: its code would differ from Linux's" % n)
        shutil.copy2(os.path.join(b, n), os.path.join(top, "llvm", "bin", n))
    shutil.copytree(os.path.join(build, "lib", "clang", "19", "include"), os.path.join(top, "llvm", "lib", "clang", "19", "include"))
    lic = os.path.join(top, "llvm", "LICENSES")
    os.makedirs(lic)
    for name in ("LLVM-LICENSE.TXT", "GCC-COPYING.RUNTIME", "winpthreads-COPYING"):
        url, want = LICENSE_TEXTS[name]
        DOWNLOADS["licence " + name] = (url, want)
        shutil.copyfile(fetch("licence " + name, cache, os.path.join(cache, "licenses", name)), os.path.join(lic, name))
    with open(os.path.join(top, "llvm", "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(LLVM_BUILT_README)
    say("llvm: the static build, %s" % ", ".join(LLVM_TOOLS))


def launcher(top, mingw):
    w = os.path.join(HERE, "window")
    res = os.path.join(top, "launcher.res.o")
    run([os.path.join(mingw, "bin", "x86_64-w64-mingw32-windres"), os.path.join(w, "launcher.rc"), "-O", "coff", "-o", res], cwd=w)
    run([os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang"), "-O2", "-municode", "-mwindows", "-static", "-Wall", "-Werror",
         "-ffile-prefix-map=%s=." % ROOT,
         os.path.join(w, "launcher.c"), res, "-o", os.path.join(top, "Grapnel.exe")])
    os.remove(res)


CONSOLE = r"""@echo off
rem Grapnel builder without the window, in this console: what grapnel_build.py takes, for instance
rem   Grapnel-console.cmd --check
rem   Grapnel-console.cmd --nup "D:\game.nup" --out "C:\Grapnel"
"%~dp0python\python.exe" -I -u "%~dp0app\scripts\grapnel-builder\grapnel_build.py" --toolchain "%~dp0toolchain" --llvm "%~dp0llvm" --python "%~dp0python\python.exe" %*
"""

LISEZMOI = """Grapnel %(version)s : le constructeur pour Windows
===================================================

Grapnel fabrique, sur votre PC, le titre PS5 « Grapnel » (GRPN00001) à partir de VOTRE propre fichier du jeu (le .nup
de Zelda Ocarina of Time, Nintendo Switch 2 Edition, avec sa mise à jour v131072). Ce dossier ne contient aucun fichier
du jeu.

Démarrer
--------
1. Décompressez tout le zip (clic droit, « Extraire tout… ») dans un dossier à vous au chemin court, par exemple
   C:\\Grapnel-Builder. Pas besoin de droits d'administrateur ni d'installation.
2. Double-cliquez sur Grapnel.exe.
3. Suivez l'assistant : 1 votre jeu (.nup), 2 les options (dossier de sortie), 3 la vérification (toute seule), 4 la
   construction du jeu (25 à 60 minutes selon le PC ; 12 Go de disque libre, 8 Go de mémoire au moins, 16 conseillés),
   5 les shaders, 6 l'envoi à la console. Le journal est caché : « Journal + » l'affiche.

Les shaders (étape 5) : le PC compile pour la console tous les shaders du jeu, dans le cache de son pilote (15 à 45
minutes) ; la console n'a plus rien à compiler en jouant. Le cache fait sous Windows est celui que fait Linux.

L'envoi à la console (étape 6, facultative) : l'adresse IP de la console, puis « Envoyer ». Il passe par FTP : le serveur
FTP de la console s'il y en a un (port 2121), sinon ftpsrv, envoyé à son chargeur d'ELF (port 9021). Vos parties sont
d'abord copiées sur ce PC (dans le dossier de sortie, console-saves), la fenêtre demande avant de remplacer quoi que ce soit,
et seuls /data/homebrew/GRPN00001 et /data/grapnel sont touchés. Un envoi coupé reprend là où il en était.

Le message de Windows (SmartScreen) ou de l'antivirus
-------------------------------------------------------
Grapnel.exe n'est pas signé (une signature coûte cher et ce projet est bénévole). Au premier lancement, Windows peut
afficher « Windows a protégé votre ordinateur » : cliquez sur « Informations complémentaires », puis « Exécuter quand
même ». Un antivirus peut aussi examiner ou bloquer les programmes du dossier (clang.exe, ld.lld.exe,
zbic_unpack.exe, ps5-native-tool.exe) : ils servent à compiler le jeu pour la console. S'il en retire un, la fenêtre
le dit (« outils incomplets ») : rétablissez le fichier ou ajoutez une exclusion pour ce dossier, puis décompressez à
nouveau.

Ce que la construction produit (dans le dossier de sortie)
-----------------------------------------------------------
homebrew\\GRPN00001 (le titre), data\\grapnel (les données), INSTALL.fr.txt (comment copier sur la console). Tout cela
vient de votre jeu : NE LE PARTAGEZ JAMAIS. Le dossier grapnel-work est le travail de la construction : il permet de
reprendre une construction interrompue ; effacez-le quand vous n'en avez plus besoin (environ 6 Go).

Sans la fenêtre : Grapnel-console.cmd --check, ou Grapnel-console.cmd --nup <fichier.nup> --out <dossier>.
Licences de chaque partie : NOTICE.txt et LICENSES\\.
"""

README = """Grapnel %(version)s: the builder for Windows
=========================================

Grapnel makes, on your PC, the PS5 title "Grapnel" (GRPN00001) from YOUR OWN copy of the game (the .nup of Zelda Ocarina
of Time, Nintendo Switch 2 Edition, with its update v131072). This folder holds no game file.

1. Unpack the whole zip (right click, "Extract All...") into a folder of yours with a short path, C:\\Grapnel-Builder for
   instance. No administrator rights, nothing to install.
2. Double-click Grapnel.exe.
3. Follow the assistant: 1 your game (.nup), 2 the options (output folder), 3 the check (it runs by itself), 4 the game's
   build (25 to 60 minutes; 12 GB of free disk, 8 GB of memory at least, 16 advised), 5 the shaders, 6 sending to the
   console. The log is hidden: "Log +" shows it.

The shaders (step 5): the PC compiles every shader of the game for the console, into its driver's cache (15 to 45 minutes):
the console then has nothing to compile as you play. The cache made on Windows is the one Linux makes.

Sending to the console (step 6, optional): the console's IP address, then "Send". It goes over FTP: the console's FTP
server if one runs (port 2121), else ftpsrv, sent to its ELF loader (port 9021). Your saves are copied to this PC first (in
the output folder, console-saves), the window asks before anything is replaced, and only /data/homebrew/GRPN00001 and
/data/grapnel are touched. A send cut short carries on where it stopped.

Windows' SmartScreen or an antivirus: Grapnel.exe is not signed. If Windows says "Windows protected your PC", click "More
info", then "Run anyway". An antivirus may scan or block the folder's programs (clang.exe, ld.lld.exe, zbic_unpack.exe,
ps5-native-tool.exe), which compile the game for the console: if it takes one away, the window says the tools are
incomplete; restore it or exclude this folder, and unpack again.

What the build makes comes from your game: never share it. grapnel-work in the output folder lets a stopped build carry on;
delete it when you no longer need it (about 6 GB). Without the window: Grapnel-console.cmd --check, or
Grapnel-console.cmd --nup <file.nup> --out <folder>. Every part's licence: NOTICE.txt and LICENSES\\.
"""

NOTICE = """Grapnel builder %(version)s for Windows: what is in this folder, and the licence of each part
=========================================================================================

No game file and nothing made from one is in this folder. Each part keeps its own licence:

Grapnel.exe
    The window's start. GPL-3.0-or-later. Source: app\\scripts\\grapnel-builder\\window\\launcher.c (its icon and
    resources beside it). Built with llvm-mingw.
app\\
    Grapnel (the PS5 port of Longshot / Hookshot, the Zelda Ocarina of Time recompilation): GPL-3.0-or-later
    (LICENSES\\GPL-3.0.txt = app\\LICENSE). Version: app\\VERSION. The third-party parts under app\\third_party keep their
    licences (each folder's LICENSE and PROVENANCE.md): among them a64-kit and suyu's translator (GPL-2.0-or-later and
    GPL-3.0), Eden's shader recompiler (GPL-2.0-or-later / GPL-3.0-or-later), Dear ImGui (MIT), libopus (BSD-3-Clause), zstd
    (BSD), PS5_MihawkGen (GPL-3.0-or-later), Libertinus (OFL-1.1), Kenney's input prompts (CC0).
app\\nx2\\zbic_unpack\\zbic_unpack.exe
    A separate program, GPL-2.0 (LICENSES\\GPL-2.0.txt): Atmosphere-NX's zstd with ZBIC support (app\\third_party\\zbic) and
    app\\nx2\\zbic_unpack\\zbic_unpack.c. The builder runs it and exchanges bytes with it through pipes only.
app\\third_party\\a64-kit\\lift\\a64emit.dll
    The translator's instruction emitter: suyu's arm64_to_c.h (GPL-2.0-or-later) with a64-kit's a64emit.cpp; source beside it.
python\\
    CPython %(python)s for Windows (python-build-standalone %(pbs)s, astral-sh): the PSF License and its parts' licences
    (python\\LICENSE.txt). Packages in python\\Lib\\site-packages, each with its licence in its .dist-info folder: numpy
    (BSD-3-Clause), capstone (BSD-3-Clause), lz4 (BSD-3-Clause), zstandard (BSD-3-Clause), fonttools (MIT), Pillow
    (MIT-CMU), astc-encoder-py (MIT; ARM's astc-encoder Apache-2.0), texture2ddecoder (MIT), archspec (Apache-2.0 or MIT).
llvm\\
%(llvm_notice)s
toolchain\\
    The console toolchain: the PS5 payload SDK (GPL-3.0-or-later), PS5_Vulkan's link parts and ps5-native-tool.exe
    (GPL-3.0-or-later, sources in toolchain\\ps5_vulkan\\tooling\\native; zlib inside, zlib licence), RADV for the PS5 (MIT),
    Khronos and VMA headers (Apache-2.0 / MIT), dav1d (BSD-2-Clause), compiler-rt's builtins (Apache-2.0 WITH
    LLVM-exception), ps5_recipe.json (GPL-3.0-or-later); the shaders' preparation for Windows (toolchain\\prepare: Grapnel's
    host and renderer, GPL-3.0-or-later; PS5_Mesa's RADV built for Windows, MIT; LLVM's libc++ and MinGW-w64's runtime inside,
    their licences in toolchain\\prepare\\LICENSES); ftpsrv-ps5.elf, the FTP server step 6 may send to the console
    (github.com/ps5-payload-dev/ftpsrv, GPL-3.0-or-later, its sources in toolchain\\ftpsrv\\src). Details and sources:
    toolchain\\NOTICE.md.

Ideas taken, with thanks, from heydemoura's Longshot PS5 Builder (github.com/heydemoura/PS5_Longshot, GPL-3.0): a
portable Windows folder packed on Linux, a launcher program, the toolchain's link recipe recorded as data.
"""


def this_pc():
    """What names this PC: its home folder, user name and host name, and its own network addresses (with the console's, which
    the port's notes name: any address of the PC's networks is refused)"""
    import socket
    words = {os.path.expanduser("~").encode(), os.path.basename(os.path.expanduser("~")).encode(), socket.gethostname().encode()}
    words |= {w.capitalize() for w in words}
    nets = set()
    r = subprocess.run(["hostname", "-I"], capture_output=True, text=True)
    for ip in r.stdout.split():
        if ip.count(".") == 3:
            words.add(ip.encode())
            nets.add(ip.rsplit(".", 1)[0].encode() + b".")
    return words, nets


def privacy_and_game_check(top):
    """Nothing of this PC (its home folder, user or host name, an address of its networks, the console's among them; a private
    network address in our own files) and nothing that looks like a game file. Examples in Python's library and the SDK's
    network headers (10.0.0.0...) are other people's text, not ours: only our files are held to the general rule"""
    words, nets = this_pc()
    bad = []
    for d, dirs, fs in os.walk(top):
        for n in fs:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, top).replace(os.sep, "/")
            if GAME_NAMES.search(rel) or os.path.getsize(p) > 200 << 20:
                bad.append("%s: a game file's name or size" % rel)
                continue
            with open(p, "rb") as f:
                data = f.read()
            named = [w.decode() for w in words if w in data]
            for m in PRIVATE_NET.finditer(data):
                if any(m.group().startswith(n) for n in nets) or rel.startswith(OURS):
                    named.append(m.group().decode())
            if named:
                bad.append("%s: names this PC or its network (%s)" % (rel, ", ".join(sorted(set(named)))))
        if os.path.basename(d) == "prompts":
            bad.append("%s: a prompts folder" % os.path.relpath(d, top))
    if bad:
        sys.exit("package: not made:\n  " + "\n  ".join(bad[:30]))


def make_zip(top, path):
    """The folder in a zip (its files in name order, one date): its one top folder is Grapnel"""
    names = []
    for d, dirs, fs in os.walk(top):
        dirs.sort()
        for n in sorted(fs):
            names.append(os.path.join(d, n))
    base = os.path.dirname(top)
    with zipfile.ZipFile(path + ".part", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in names:
            info = zipfile.ZipInfo(os.path.relpath(p, base).replace(os.sep, "/"), date_time=(2026, 11, 23, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(p, "rb") as f:
                z.writestr(info, f.read(), compresslevel=9)
    os.replace(path + ".part", path)
    return names


def main():
    ap = argparse.ArgumentParser(description="Grapnel builder for Windows: the portable zip, made on Linux")
    ap.add_argument("--version", default="0.2-test1", help="the package's name (the build's own version is git describe)")
    ap.add_argument("--out", help="default dist/next/grapnel-windows-<version>")
    ap.add_argument("--stage", default=os.path.join(PS5, "build", "grapnel-windows", "stage"))
    ap.add_argument("--cache", default=os.path.join(PS5, "build", "grapnel-windows", "downloads"))
    ap.add_argument("--toolchain", default=os.path.join(PS5, "build", "grapnel-windows", "toolchain", "grapnel-toolchain-0.3-windows-x64"),
                    help="make_toolchain.py --system windows-x64's folder")
    ap.add_argument("--mingw", default=os.path.expanduser("~/.local/opt/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64"),
                    help="llvm-mingw for Linux: builds Grapnel.exe and the two helper programs")
    ap.add_argument("--readobj", default="/usr/lib/llvm-19/bin/llvm-readobj", help="llvm-readobj (the Windows DLLs' imports)")
    ap.add_argument("--llvm-build", help="(0.3-test3) make_windows_llvm.sh's build folder: its static LLVM instead of WinLibs'")
    ap.add_argument("--allow-dirty", action="store_true", help="(tests) package a checkout with uncommitted changes")
    ap.add_argument("--no-zip", action="store_true", help="stop at the folder (<stage>/Grapnel)")
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(PS5, "dist", "next", "grapnel-windows-" + a.version))
    top = os.path.join(os.path.abspath(a.stage), "Grapnel")
    if not os.path.isfile(os.path.join(a.toolchain, "ps5_recipe.json")):
        sys.exit("package: %s is no Windows toolchain (make_toolchain.py --system windows-x64 --out ...)" % a.toolchain)
    shutil.rmtree(top, ignore_errors=True)
    os.makedirs(top)
    say("app (git archive HEAD) and its helper programs")
    version = stage_app(os.path.join(top, "app"), a.mingw, a.allow_dirty)
    say("python, wheels")
    stage_python(top, a.cache)
    say("llvm")
    if a.llvm_build:
        stage_llvm_built(top, os.path.abspath(a.llvm_build), a.cache, a.readobj)
    else:
        stage_llvm(top, a.cache, a.readobj)
    say("toolchain")
    shutil.copytree(a.toolchain, os.path.join(top, "toolchain"))
    say("Grapnel.exe")
    launcher(top, a.mingw)
    fill = {"version": "%s (%s)" % (a.version, version), "python": "3.13.18", "pbs": "20261110",
            "llvm_notice": LLVM_NOTICE_BUILT if a.llvm_build else LLVM_NOTICE_WINLIBS}
    for name, text in (("Grapnel-console.cmd", CONSOLE), ("LISEZMOI.txt", LISEZMOI % fill), ("README.txt", README % fill),
                       ("NOTICE.txt", NOTICE % fill)):
        with open(os.path.join(top, name), "w", encoding="utf-8-sig" if name.endswith(".txt") else "utf-8", newline="\r\n") as f:
            f.write(text)
    os.makedirs(os.path.join(top, "LICENSES"))
    shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(top, "LICENSES", "GPL-3.0.txt"))
    shutil.copyfile(os.path.join(ROOT, "third_party", "zbic", "LICENSE"), os.path.join(top, "LICENSES", "GPL-2.0.txt"))
    privacy_and_game_check(top)
    longest = max(len(os.path.relpath(os.path.join(d, n), os.path.dirname(top))) for d, _, fs in os.walk(top) for n in fs)
    total = sum(os.path.getsize(os.path.join(d, n)) for d, _, fs in os.walk(top) for n in fs)
    say("%s: %.0f MB, longest path inside %d characters" % (top, total / 1e6, longest))
    if a.no_zip:
        return 0
    os.makedirs(out, exist_ok=True)
    zpath = os.path.join(out, "Grapnel-Builder-%s-windows-x64.zip" % a.version)
    names = make_zip(top, zpath)
    say("%s: %.0f MB, %d files" % (zpath, os.path.getsize(zpath) / 1e6, len(names)))
    with open(os.path.join(out, "MANIFEST.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Grapnel builder %s for Windows (checkout %s)\n\n" % (a.version, version))
        f.write("%-56s %12s  %10s  %s\n" % ("file", "bytes", "CRC32", "SHA-256"))
        f.write("%-56s %12d  %10d  %s\n\n" % (os.path.basename(zpath), os.path.getsize(zpath), crc32(zpath), sha256(zpath)))
        f.write("What the zip holds (%d files, %d bytes unpacked; path, bytes, CRC32):\n" % (len(names), total))
        for p in names:
            f.write("%s %d %d\n" % (os.path.relpath(p, os.path.dirname(top)).replace(os.sep, "/"), os.path.getsize(p), crc32(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
