#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""(grapnel, Windows) The guards around the driver's compiles take only the driver's own faults (gpu/vk/vk_draw.cpp
driver_fault_filter). Under Wine a corrupt heap faulted in ntdll's heap_free_block with the heap's lock held; the guard's
__except (1) took that fault for the driver's, and the preparation went on on a broken heap, or waited for that lock for ever.

The filter, cut out of vk_draw.cpp and compiled here over stand-ins for the few Windows types and calls it uses:
- a fault whose instruction is in ntdll, kernelbase or ucrtbase is left to end the process (EXCEPTION_CONTINUE_SEARCH);
- any other is the driver's, and the guard leaves that program out as before (EXCEPTION_EXECUTE_HANDLER);
- each is written to stderr first: the code, the instruction (module + offset when it is a system module's), read or write,
  and the address touched.
And in the source: the three guards use it, none takes every fault any more, and the filter allocates nothing (the heap it
runs over may be the broken one)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srccut import block, between  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "gpu" / "vk" / "vk_draw.cpp"

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
typedef long LONG;
typedef unsigned long DWORD;
typedef void* HANDLE;
typedef unsigned long long ULONG_PTR;
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define STD_ERROR_HANDLE ((DWORD)-12)
struct EXCEPTION_RECORD { DWORD ExceptionCode; DWORD ExceptionFlags; void* ExceptionRecord; void* ExceptionAddress;
                          DWORD NumberParameters; ULONG_PTR ExceptionInformation[15]; };
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; void* ContextRecord; };
static std::string g_out;
static HANDLE GetStdHandle(DWORD) { return (HANDLE)1; }
static int WriteFile(HANDLE, const void* b, DWORD n, DWORD* w, void*) { g_out.append((const char*)b, n); *w = n; return 1; }
%(struct)s
ModRange g_sys_mods[3];
%(put_hex)s
%(put_str)s
%(filter)s
static int failures = 0;
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %%s: %%s\n", what, g_out.c_str()); failures++; } }
int main() {
    g_sys_mods[0].lo = 0x6fffffc20000ull, g_sys_mods[0].hi = 0x6ffffffec000ull;
    std::strcpy(g_sys_mods[0].name, "ntdll.dll");
    g_sys_mods[2].lo = 0x6ffffed30000ull, g_sys_mods[2].hi = 0x6ffffee41000ull;
    std::strcpy(g_sys_mods[2].name, "ucrtbase.dll");
    // (24/11/2026, under Wine: heap_free_block's movq %%rcx,(%%rax) writing to 0)
    EXCEPTION_RECORD r = {EXCEPTION_ACCESS_VIOLATION, 0, nullptr, (void*)0x6fffffc6f21aull, 2, {1, 0}};
    EXCEPTION_POINTERS ep = {&r, nullptr};
    check(driver_fault_filter(&ep, "a graphics pipeline") == EXCEPTION_CONTINUE_SEARCH, "ntdll: not caught");
    check(g_out.find("fault in a graphics pipeline: code 0x00000000c0000005 at 0x00006fffffc6f21a (ntdll.dll+0x000000000004f21a)"
                     " writing 0x0000000000000000: not the driver's, the process ends\n") != std::string::npos, "ntdll: the line");
    g_out.clear();
    r.ExceptionAddress = (void*)0x6ffffed40f15ull;
    r.ExceptionInformation[0] = 0, r.ExceptionInformation[1] = 0x12e00110000ull;
    check(driver_fault_filter(&ep, "a compute pipeline") == EXCEPTION_CONTINUE_SEARCH, "ucrtbase: not caught");
    check(g_out.find("(ucrtbase.dll+0x0000000000010f15) reading 0x0000012e00110000: not the driver's") != std::string::npos,
          "ucrtbase: the line");
    g_out.clear();
    r.ExceptionAddress = (void*)0x180123456ull;   // (the driver's own code)
    check(driver_fault_filter(&ep, "a shader module") == EXCEPTION_EXECUTE_HANDLER, "driver: caught");
    check(g_out.find("at 0x0000000180123456 reading 0x0000012e00110000: the driver's, that one is left out\n") != std::string::npos,
          "driver: the line");
    g_out.clear();
    r.ExceptionCode = 0xC00000FDu, r.NumberParameters = 0;   // (a stack overflow in the driver: no address touched)
    check(driver_fault_filter(&ep, "a graphics pipeline") == EXCEPTION_EXECUTE_HANDLER, "overflow: caught");
    check(g_out.find("code 0x00000000c00000fd at 0x0000000180123456: the driver's") != std::string::npos, "overflow: the line");
    return failures ? 1 : 0;
}
'''


def main():
    text = SRC.read_text(encoding="utf-8")
    failed = []
    # (the source: the three guards use the filter, none takes every fault, the filter allocates nothing)
    if "__except (1)" in text:
        failed.append("a guard in vk_draw.cpp still takes every fault: __except (1)")
    for what in ("a graphics pipeline", "a compute pipeline", "a shader module"):
        if '__except (driver_fault_filter(GetExceptionInformation(), "%s"))' % what not in text:
            failed.append("the guard of %s does not use driver_fault_filter" % what)
    marker = "LONG driver_fault_filter(EXCEPTION_POINTERS* ep, const char* what) {"
    if marker not in text:
        failed.append("vk_draw.cpp has no driver_fault_filter: every fault in a guard is taken for the driver's")
    filt = block(text, marker) if marker in text else ""
    for word in ("snprintf", "malloc", "new ", "std::string", "std::vector", "logf", "printf", "GetModuleHandle"):
        if word in filt:
            failed.append("the filter calls %s: it must not allocate nor take a lock" % word)
    if text.count("sys_mods_once();\n    __try {") != 3:
        failed.append("the system modules' ranges are not taken before each guard's __try")
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not filt:
        pass
    elif not cxx:
        print("skipped: no C++ compiler")
    else:
        parts = {"struct": between(text, "struct ModRange {", "ModRange g_sys_mods[3];"),
                 "put_hex": block(text, "char* put_hex(char* o, uint64_t v) {"),
                 "put_str": between(text, "char* put_str(char* o, const char* s)", "LONG driver_fault_filter("),
                 "filter": filt}
        with tempfile.TemporaryDirectory() as d:
            src, exe = os.path.join(d, "filter.cpp"), os.path.join(d, "filter")
            with open(src, "w", encoding="utf-8") as f:
                f.write(HARNESS % parts)
            r = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-o", exe, src], capture_output=True, text=True)
            if r.returncode:
                failed.append("the filter does not compile alone:\n" + r.stderr[-2000:])
            else:
                r = subprocess.run([exe], capture_output=True, text=True)
                if r.returncode:
                    failed.append("the filter:\n" + r.stdout[-2000:])
    for f in failed:
        print("FAIL:", f)
    if not failed:
        print("ok: the guards take the driver's faults alone")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
