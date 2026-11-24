"""(ps5) RX_TARGET=ps5: the game and its runtime built for a jailbroken PS5, with the payload SDK's compilers.

The console's toolchain is the PS5 payload SDK fork (PS5_PayloadSDK, installed in a title's .deps/native/ps5-payload-sdk):
`bin/prospero-clang` and `bin/prospero-clang++` drive the host's clang for `x86_64-sie-ps5` (it defines __PROSPERO__),
with the SDK as sysroot and the four flags the console needs (-femulated-tls, -fno-plt, -fno-stack-protector,
-fdenormal-fp-math=ieee). `bin/prospero-ar` makes the static archives. The final link (eboot.bin) is RADV's recipe
from PS5_Vulkan (tools/radv-link.sh) as PS5_VulkanTemplate's ps5/tools/link-title.sh uses it: link_title() below, which
build.py runs after the archives (the game's title, or with --runtime the boot test's).

Where the SDK is, in order: PS5_PAYLOAD_SDK, HOOKSHOT_PS5_SDK, or the first ps5-stack/PS5_VulkanTemplate's
.deps/native/ps5-payload-sdk found in a folder above this repository (hookshot-ps5/ps5-stack). PS5_VULKAN or
ps5-stack/PS5_Vulkan the same way, for the link.

What is fixed for the console, not probed on the PC that builds:
- -march=x86-64-v3 -mtune=znver2: the PS5's CPU is an 8-core Zen 2, which runs x86-64-v3 (AVX2, FMA, BMI1/2, MOVBE).
  The PC's own CPU is never asked (nx2/host_cpu.py is not used): the game runs on the console, not here.
- A64_HOST_BASE=0x8000000000 (512 GiB): the guest window. The Linux and Windows builds put it at 16 TiB, which the
  console never grants (PS5_PayloadSDK platform/docs/PROBE.md, "Virtual space": nothing at or above 1 TiB). The
  window's 512 GiB end exactly at 1 TiB, above the platform heap (0x20_0000_0000, 16 GiB) and the Vulkan driver's
  device memory (0x40_0000_0000). docs/ps5-port.md, "Memory".
- -fno-omit-frame-pointer for the runtime: the console's crash backtraces are frame-pointer walks.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MARCH = "x86-64-v3"
MTUNE = "znver2"
GUEST_BASE = 0x8000000000   # (A64_HOST_BASE on the console: 512 GiB, the window ends at 1 TiB)


def active():
    """RX_TARGET=ps5"""
    return os.environ.get("RX_TARGET", "").lower() == "ps5"


def _above(*parts):
    """The first <folder above this repository>/<parts> that exists, or None"""
    d = ROOT
    while True:
        p = os.path.join(d, *parts)
        if os.path.exists(p):
            return p
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def sdk():
    """The payload SDK's root (bin/prospero-clang, target/include, target/lib/libps5platform.a)"""
    for e in ("PS5_PAYLOAD_SDK", "HOOKSHOT_PS5_SDK"):
        if os.environ.get(e):
            return os.environ[e]
    found = _above("ps5-stack", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk")
    if not found:
        raise SystemExit("RX_TARGET=ps5: no payload SDK: set PS5_PAYLOAD_SDK to the SDK fork's install "
                         "(a title's .deps/native/ps5-payload-sdk)")
    return found


def vulkan():
    """PS5_Vulkan's checkout (the RADV archive, tools/radv-link.sh, tooling/native, runtime/libc.prx), or None"""
    return os.environ.get("PS5_VULKAN") or _above("ps5-stack", "PS5_Vulkan")


TOOLS = {"clang": "prospero-clang", "clang++": "prospero-clang++", "ar": "prospero-ar", "llvm-ar": "prospero-ar",
         "ranlib": "prospero-ranlib", "lld": "prospero-lld", "nm": "llvm-nm", "llvm-nm": "llvm-nm"}


# ---- (grapnel) the console toolchain without a shell: Windows, and the same path on Linux ----------------------------------
# The payload SDK's prospero-* wrappers, PS5_Vulkan's tooling/prospero-clang18 and tools/radv-link.sh are bash and sh scripts,
# which a player's Windows has not. With PS5_RECIPE (a Grapnel toolchain folder's ps5_recipe.json) and PS5_LLVM_BIN (LLVM
# 19.1.7's bin folder), the LLVM tools are run directly, with the very arguments those scripts give them: the recipe recorded
# them once, on Linux, by running the scripts themselves (scripts/grapnel-builder/make_toolchain.py, record_recipe). Its paths
# are written {sdk}, {vk}, {radv}, {builtins}, {toolchain}, filled in here. Without PS5_RECIPE nothing changes: the wrappers.
DIRECT_TOOLS = {"clang": "clang", "clang++": "clang++", "ar": "llvm-ar", "llvm-ar": "llvm-ar", "ranlib": "llvm-ranlib",
                "lld": "ld.lld", "nm": "llvm-nm", "llvm-nm": "llvm-nm"}
_RECIPE = {}


def direct():
    """The toolchain's recipe drives the LLVM tools directly (PS5_RECIPE): no bash"""
    return bool(os.environ.get("PS5_RECIPE"))


def recipe():
    """PS5_RECIPE's recipe, its placeholders filled in with this PC's paths (once per process)"""
    import json
    path = os.path.abspath(os.environ["PS5_RECIPE"])
    if path not in _RECIPE:
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        if raw.get("format") != 1:
            raise SystemExit("PS5_RECIPE: %s is not a recipe this build reads (format %r)" % (path, raw.get("format")))
        top = os.path.dirname(path)
        _RECIPE[path] = fill_places(raw, {"toolchain": top, "sdk": os.path.join(top, "sdk"), "vk": os.path.join(top, "ps5_vulkan"),
                                          "radv": os.path.join(top, *raw["radv"].split("/")),
                                          "builtins": os.path.join(top, *raw["builtins"].split("/"))})
    return _RECIPE[path]


def fill_places(v, places):
    """The recipe's {name} placeholders filled in, in strings, lists and dicts; the "/" after a placeholder made this system's
    separator. A placeholder not in places stays as it is ({map}: link_title's own)"""
    import re
    if isinstance(v, str):
        return re.sub(r"\{(\w+)\}(\S*)", lambda m: places[m.group(1)] + m.group(2).replace("/", os.sep) if m.group(1) in places
                      else m.group(0), v)
    if isinstance(v, list):
        return [fill_places(x, places) for x in v]
    if isinstance(v, dict):
        return {k: fill_places(x, places) for k, x in v.items()}
    return v


def _exe(name):
    return name + (".exe" if os.name == "nt" else "")


def llvm_bin():
    b = os.environ.get("PS5_LLVM_BIN") or os.path.dirname(os.environ.get("PS5_CLANG") or "")
    if not b:
        raise SystemExit("PS5_RECIPE: PS5_LLVM_BIN (LLVM 19.1.7's bin folder) is not set")
    return b


def find(tool):
    if direct():
        # (clang++ needs no file of its own: command() runs clang in its C++ mode; Windows' LLVM has the one clang.exe here)
        path = os.path.join(llvm_bin(), _exe(DIRECT_TOOLS.get(tool, tool)))
        if tool != "clang++" and not os.path.isfile(path):
            raise SystemExit("RX_TARGET=ps5: %s not found in LLVM's bin folder (%s)" % (os.path.basename(path), llvm_bin()))
        return path
    path = os.path.join(sdk(), "bin", TOOLS.get(tool, tool))
    if not os.path.isfile(path):
        raise SystemExit("RX_TARGET=ps5: %s not found in the payload SDK (%s)" % (TOOLS.get(tool, tool), sdk()))
    return path


def _tool_name(path):
    n = os.path.basename(path).lower()
    return n[:-4] if n.endswith(".exe") else n


def command(tool, args):
    """The command that runs a tool find() gave on args: [tool] + args with the SDK's wrappers; in direct mode, the LLVM tool with
    the arguments the wrapper would have put around args (the recipe's). Direct clang compiles only (-c, -E, -S, -###): a link
    goes through lld (link_title), never through the compiler's driver, which would look for the wrapper prospero-lld"""
    args = list(args)
    if not direct():
        return [tool] + args
    name, r = _tool_name(tool), recipe()
    if name in ("clang", "clang++"):
        if not any(a in ("-c", "-E", "-S", "-###") for a in args):
            raise RuntimeError("direct toolchain: %s links nothing (%s): link with lld" % (name, " ".join(args[-4:])))
        w = r["compile"]["c++" if name == "clang++" else "c"]
        clang = os.path.join(llvm_bin(), _exe("clang"))
        return [clang] + forward_slashes((["--driver-mode=g++"] if name == "clang++" else []) + w["before"] + args + w["after"])
    if name == "ld.lld":
        # (prospero-lld: -pie unless --shared, -r or --static; the SDK's linker script unless the command gives one. The recipe
        # has the wrapper's arguments for each case the build uses)
        pie = not any(a in ("--shared", "-r", "--static") for a in args)
        script = not any(a in ("--script", "-T") for a in args)
        case = ("pie" if pie else "nopie") + ("+script" if script else "")
        w = r["lld"].get(case)
        if w is None:
            raise RuntimeError("direct toolchain: the recipe has no prospero-lld case %s" % case)
        if os.name == "nt" and not any(a.startswith("--threads") for a in args):
            args = ["--threads=1"] + args   # (Windows: link_threads())
        return [tool] + w["before"] + args + w["after"]
    return [tool] + args   # (llvm-ar, llvm-nm: the wrappers run them as they are)


def link_threads():
    """lld's threads: HOOKSHOT_PS5_LINK_THREADS, else 3 (it takes every core otherwise; the build's job count is the machine's
    share). (grapnel) Windows: 1. Under Wine the Windows builder's ld.lld (WinLibs', on winpthreads) hung for good 10 links of
    10 with its threads, and none of 10 with one (23/11/2026); a real Windows was not tried. The output is the same whatever the
    count; one thread costs the eboot's link about a minute"""
    raw = os.environ.get("HOOKSHOT_PS5_LINK_THREADS")
    return max(1, int(raw)) if raw else (1 if os.name == "nt" else 3)


def crt_command(args):
    """PS5_Vulkan's tooling/prospero-clang18 (the CRT and the AGC link stubs): the host's clang with the repository's target flags"""
    if not direct():
        return ["sh", os.path.join(vulkan(), "tooling", "prospero-clang18")] + list(args)
    return [os.path.join(llvm_bin(), _exe("clang"))] + forward_slashes(recipe()["crt"]["before"] + list(args))


def forward_slashes(args):
    """(Windows) a compile's paths written with "/", as on Linux: __FILE__ is the path clang was given (the source file's, a
    header's search folder's), -ffile-prefix-map takes the checkout's folder off its front, and for a PS5 target clang then
    normalises it the POSIX way only: "C:\\x\\nx2\\..\\gpu\\a.cpp" stayed ".\\nx2\\..\\gpu\\a.cpp" where Linux wrote
    "nx2/../gpu/a.cpp", and the eboot's assertion strings differed. Between a folder and a header clang puts its own preferred
    separator: "/" in a MinGW-built LLVM, which the Windows builder ships (WinLibs', built with GCC and libstdc++ as Linux's
    clang is: scripts/grapnel-builder/make_windows_package.py), so the strings come out the same as Linux's. Macro definitions
    (-D, -U) are left as they are"""
    if os.sep != "\\":
        return list(args)
    return [a if a.startswith(("-D", "-U")) else a.replace("\\", "/") for a in args]


def target():
    """What every compile adds (the wrappers already give the target, the sysroot and the console's four flags).
    (grapnel) -ffile-prefix-map: the checkout's own folder written as "." wherever a compile would write it (__FILE__ in Eden's
    and suyu's assertions, 5 strings in the eboot). Without it every eboot held the path of the folder it was built from (the
    player's user name with it), and two players, or two folders, made different eboots. object_key's flag_text writes this
    folder as <root>, so the objects' keys do not change with it either"""
    return ["-DA64_HOST_BASE=0x%xULL" % GUEST_BASE, "-ffile-prefix-map=%s=." % ROOT]


def cpu():
    """The console's CPU, never the build PC's"""
    return ["-march=" + MARCH, "-mtune=" + MTUNE]


def runtime_flags():
    """The runtime's C and C++ (host/, the renderer, opus): frame pointers for the console's backtraces, sections for
    the eboot's --gc-sections"""
    return ["-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections"]


def archive(lib, objs, rsp=None):
    """A static archive of these objects (replaced whole: an object left over from an older build is not kept)"""
    if os.path.exists(lib):
        os.remove(lib)
    rsp = rsp or lib + ".rsp"
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"%s"' % o for o in objs))
    subprocess.run([find("ar"), "rcsD", lib, "@" + rsp], check=True)
    return lib


# (lot 80) the port's public name, Grapnel: its own title id (PPSA99145 "Hookshot PS5" before; free on the console: PPSA99141
# the probe, PPSA99144 the GPU smoke title, PPSA99147 the platform probe), the name under its tile on the home screen. The
# game's data folder follows: /data/grapnel (host/posix/ps5_data_folder.h, which takes /data/hookshot over)
TITLE_ID = "GRPN00001"
TITLE_NAME = "Grapnel"
CONTENT_LABEL = "GRAPNELPS5000000"   # (contentId's 16 characters after the title id)


GAME_RADV = ("mesa-work", "game-build", "radv-release-sync", "lib", "libvulkan_radeon.ps5.a")   # (the lots' since lot 57)


def dav1d():
    """dav1d cross-built for the console, for the game's AV1 movies (host/posix/ps5_dav1d.c): (include folders, archive),
    or None. HOOKSHOT_PS5_DAV1D=<its meson build folder> (0: none); else hookshot-ps5's build/dav1d-ps5 beside its source
    third_party/dav1d"""
    e = os.environ.get("HOOKSHOT_PS5_DAV1D", "")
    if e == "0":
        return None
    build = e or _above("build", "dav1d-ps5")
    # (grapnel) HOOKSHOT_PS5_DAV1D_INCLUDE=<dav1d's include/ folder>: its public headers where no source tree is above the
    # checkout (the builder's toolchain folder carries them beside the archive)
    src = os.environ.get("HOOKSHOT_PS5_DAV1D_INCLUDE") or _above("third_party", "dav1d", "include")
    if not build or not src or not os.path.exists(os.path.join(build, "src", "libdav1d.a")):
        return None
    return [src, os.path.join(build, "include")], os.path.join(build, "src", "libdav1d.a")


def radv_archive(kind="game"):
    """The RADV archive the eboot links: RADV_ARCHIVE when set; for the game, the game's RADV when it is built (hookshot-ps5's
    mesa-work/game-build/radv-release-sync: PS5_Mesa with the task-shader patch, the mesh count fix, the flat COND_EXEC and
    lot 57's narrower waits, the driver agent's; its PROVENANCE.txt); else, and for the boot test, PS5_Vulkan's release
    archive (the one the GPU smoke title ran with)"""
    if os.environ.get("RADV_ARCHIVE"):
        return os.environ["RADV_ARCHIVE"]
    stock = os.path.join(vulkan() or "", ".deps", "native", "radv-release", "lib", "libvulkan_radeon.ps5.a")
    if kind == "game":
        game = _above(*GAME_RADV)
        if game:
            return game
        print("ps5: the game's RADV (%s) is not built: PS5_Vulkan's release archive" % os.path.join(*GAME_RADV), flush=True)
    return stock


def host_clang():
    """The host's clang (19, as the SDK's wrappers drive): PS5_CLANG, else the first clang on PATH. The link recipe asks it for
    clang's builtins (__emutls_get_address)"""
    import shutil
    c = os.environ.get("PS5_CLANG") or shutil.which("clang") or shutil.which("clang-19") or shutil.which("clang-18")
    if not c:
        raise SystemExit("RX_TARGET=ps5: no clang on PATH for the link recipe: set PS5_CLANG")
    return c


FLEXIBLE_MB = 1024   # (kernel.flexibleMemorySize: the most the console grants; param_json)


def flexible_mb():
    """The title's flexible memory in MiB, param.json's kernel.flexibleMemorySize: HOOKSHOT_PS5_FLEXIBLE_MB, else 1024. The
    console's default is 448 MiB, and it charges every segment of the eboot to it at load (the platform probe PPSA99147: its
    MAP lines, and 285 MiB free at main for a 24 MB eboot with the 128 MiB table): the game's 446.8 MB of code alone is
    426 MiB. The first game launch (f7c8633, no kernel key) powered the console off at load; the Big Eboot Test PPSA97001
    (573 MB) launched with 1 GiB. 1024 MiB is the most (PS5_VulkanTemplate title-packaging.md: 2 MiB to 1 GiB accepted,
    0x80020016 above); the 576 MiB more come out of the title's direct memory. 448: the key left out.
    Sized (2026-11-09): the game's segments 496 MiB (the table out of .bss), plus ~5.4 MiB at main (libc.prx, the 2 MiB main
    stack, the system modules: the boot test's 266 MiB free = 448 - 176.6 - 5.4); after main only libc's own allocations and
    the heap's fallback take flexible memory (the platform heap, every stack, the guest window, the GPU's memory and the table
    are direct). 1024 leaves ~520 MiB; 768 would leave ~267, the headroom the boot test had at stage 8, with nothing measured
    past the renderer yet: the stage trail's first-present and 300th/600th-frame lines measure it, then 768 if they allow"""
    v = int(os.environ.get("HOOKSHOT_PS5_FLEXIBLE_MB") or FLEXIBLE_MB)
    if not 2 <= v <= 1024:
        raise SystemExit("HOOKSHOT_PS5_FLEXIBLE_MB: 2 to 1024 (MiB), not %d" % v)
    return v


def param_json(title_id=TITLE_ID, name=TITLE_NAME, label=CONTENT_LABEL):
    """sce_sys/param.json: the GPU smoke title's (PPSA99144, run on the console), another title id and name.
    downloadDataSize > 0: what /data's elevation asks of a title (PS5_PayloadSDK platform/docs/ELEVATION.md);
    attribute3 0x80040: the high frame rate bit (119.88 Hz modes allowed) the smoke title ran with;
    kernel.flexibleMemorySize: flexible_mb() (1 GiB: what the eboot's segments are charged to at load), left out at 448"""
    import json
    flexible = flexible_mb()
    return json.dumps({
        "ageLevel": {"default": 0}, "applicationCategoryType": 0, "applicationDrmType": "free", "attribute": 0,
        "attribute2": 0, "attribute3": 0x80040, "conceptId": title_id[4:], "contentBadgeType": 1,
        "contentId": "UP9000-%s_00-%s" % (title_id, label), "contentVersion": "01.000.000", "downloadDataSize": 256,
        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": name}}, "masterVersion": "01.00",
        "pubtools": {"creationDate": "2026-11-09 00:00:00", "loudnessSnd0": "-28.00", "toolVersion": "2.00"},
        "requiredSystemSoftwareVersion": "0x0000000000000000", "sdkVersion": "0x0000000000000000", "titleId": title_id,
        "versionFileUri": "", **({} if flexible == 448 else {"kernel": {"flexibleMemorySize": flexible << 20}})},
        indent=2, sort_keys=True) + "\n"


def write_param_json(path):
    """sce_sys/param.json, its lines ending in LF on every system: Windows' Python wrote CRLF (0.2-test2: 944 bytes for Linux's
    903), a file the console reads"""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(param_json())


def _run(cmd, log, **kw):
    log.write("$ " + " ".join(cmd) + "\n")
    log.flush()
    r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, **kw)
    if r.returncode:
        raise RuntimeError("%s failed (%d): see %s" % (os.path.basename(cmd[0]), r.returncode, log.name))


def _recipe(vk, s, archive, env):
    """tools/radv-link.sh's recipe (sourced, as every title that links RADV does): its three arrays"""
    script = ('source "$1/tools/radv-link.sh" && radv_link_recipe "$1" "$2" "$3" >&2 || exit 2; '
              'for a in radv_linker_script radv_link_flags radv_link_inputs; do declare -n v=$a; '
              'printf "%s\\0" "${v[@]}"; printf "\\1\\0"; done')
    r = subprocess.run(["bash", "-c", script, "recipe", vk, s, archive], capture_output=True, env=env)
    if r.returncode:
        raise RuntimeError("radv_link_recipe failed: " + r.stderr.decode(errors="replace"))
    parts, cur = [], []
    for item in r.stdout.decode().split("\0"):
        if item == "\1":
            parts.append(cur)
            cur = []
        elif item:
            cur.append(item)
    return parts   # linker script, flags, inputs


def _recipe_data(work):
    """(grapnel) radv-link.sh's three arrays as the toolchain's recipe recorded them; the version script it writes (the names it
    binds, kept local) written into this link's folder instead of the toolchain's"""
    link = recipe()["link"]
    mapfile = os.path.join(work, "radv-platform-local.map")
    with open(mapfile, "w", encoding="utf-8", newline="\n") as f:
        f.write(link["map"])
    places = {"map": mapfile}
    return fill_places(link["script"], places), fill_places(link["flags"], places), fill_places(link["inputs"], places)


LINKER_DEFINED = ("__preinit_array_", "__init_array_", "__fini_array_", "__start_", "__stop_", "__ehdr_start",
                  "__executable_start", "_DYNAMIC", "_GLOBAL_OFFSET_TABLE_", "__dso_handle", "__bss_start", "_end", "_etext",
                  "_edata", "__eh_frame", "__tls_")


# (2026-11-15) host/posix/ps5_compile_heap.c's wraps: the heap's allocations and its segments' return, the driver's compiles
COMPILE_HEAP_WRAPS = ("ps5_heap_malloc", "ps5_heap_calloc", "ps5_heap_memalign", "ps5p_heap_unmap",
                      "radv_CreateGraphicsPipelines", "radv_CreateComputePipelines", "radv_shader_create",
                      "radv_pipeline_cache_insert", "vk_pipeline_cache_lookup_object")


def _weak_undefined(paths, others):
    """Weak references our archives make that nothing linked defines: a PIE's link makes each an import (lld 19 has no
    -z nodynamic-undefined-weak), and the native tool refuses an import no module exports. On Windows and Linux they are
    0, and so here (--defsym): the thread_local init functions (_ZTH*) of constant-initialized thread_locals, and host/'s
    optional entry points (hos_ps5_boot in the game).
    others: the archives and stubs the recipe links (RADV, the platform, libc++, the SDK's modules): what they define is
    left alone, and so are the names the linker defines itself (the CRT's __init_array_start...)"""
    weak, defined = set(), set()
    for path in paths:
        for line in _nm(path):
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("w", "v"):
                weak.add(parts[1])
            elif len(parts) >= 3 and parts[1] not in ("U", "w", "v"):
                defined.add(parts[2])
    want = {n for n in weak - defined if not n.startswith(LINKER_DEFINED)}
    for path in others:
        if not want:
            break
        want -= _defined(path, path.endswith(".so"))
    return sorted(want)


DIRTY_BYTES = 1 << 27        # (a64-kit's write-watch table, a64_dirty: a mark per 4 KiB page of the 512 GiB window)
DIRTY_GAP = 64 << 20         # (between the image's .bss and the table: the native tool's dynamic-data segment goes there)


def dirty_script(work, script):
    """(ps5) The link's script: PS5_Vulkan's layout (the recipe's -T, INCLUDEd) with a64-kit's write-watch table out of the image.
    a64_dirty (128 MiB, its own input section .bss.a64_dirty: rt64/a64_rt.c is built with -fdata-sections) was .bss, which the
    console charges to the title's flexible memory at load. Its storage is discarded and the symbol placed DIRTY_GAP past the
    end of .bss, 2 MiB aligned: the lifted code reaches it RIP-relative (leaq disp32(%rip), resolved here), and the eboot's
    first code backs exactly that range with direct memory (host/posix/ps5_early.c) before anything touches it. A symbol
    assigned from a section's address stays relative to the image (a PIE), and a script's assignment replaces the object's
    definition. HOOKSHOT_PS5_DIRTY_IN_BSS=1: the recipe's script as it is (the table in .bss again, no code change needed:
    ps5_early.c finds it mapped and does nothing). Returns the -T/-L arguments to use"""
    if os.environ.get("HOOKSHOT_PS5_DIRTY_IN_BSS") == "1" or "-T" not in script:
        return list(script)
    i = script.index("-T")
    base = os.path.abspath(script[i + 1])
    ld = os.path.join(work, "hookshot-ps5.ld")
    # ((grapnel) UTF-8, which lld reads: Windows' code page wrote "Prénom" in the INCLUDE below as lld could not find it)
    with open(ld, "w", encoding="utf-8") as f:
        f.write("/* nx2/toolchain_ps5.py dirty_script: PS5_Vulkan's layout, a64-kit's write-watch table out of the image's .bss\n"
                "   (backed with direct memory at start by host/posix/ps5_early.c) */\n"
                "SECTIONS { /DISCARD/ : { *(.bss.a64_dirty) } }\n"
                "INCLUDE \"%s\"\n"
                "a64_dirty = ALIGN(ADDR(.bss) + SIZEOF(.bss) + %#x, 0x200000);\n" % (base, DIRTY_GAP))
    return script[:i] + ["-T", ld] + script[i + 2:]


def _loads(elf):
    """An ELF's PT_LOAD segments: (vaddr, filesz, memsz, flags), from its program headers"""
    import struct
    with open(elf, "rb") as f:
        h = f.read(64)
        phoff, phentsize, phnum = struct.unpack_from("<Q", h, 32)[0], *struct.unpack_from("<HH", h, 54)
        f.seek(phoff)
        table = f.read(phentsize * phnum)
    out = []
    for k in range(phnum):
        p_type, p_flags, _, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from("<IIQQQQQQ", table, k * phentsize)
        if p_type == 1:
            out.append((p_vaddr, p_filesz, p_memsz, p_flags))
    return out


def segment_report(eboot_elf, llvm_elf):
    """(ps5) The console executable's loaded segments and what the loader charges to flexible memory (every mapped one; the
    flagless dynamic-data segment the native tool appends is read at load, not kept mapped: the probe's MAP lines), with the
    write-watch table's place. Lines for the log and the build's output; raises when the table is inside the image or out of
    the code's RIP-relative reach"""
    loads = _loads(eboot_elf)
    page = lambda n: (n + 0x3fff) & ~0x3fff
    lines, charged, top = [], 0, 0
    names = {5: "code", 4: "read-only", 6: "read-write", 1: "code", 0: "no access (dynamic data)"}
    for vaddr, filesz, memsz, flags in loads:
        kind = names.get(flags & 7, "flags %#x" % flags)
        if flags & 7:
            charged += page(memsz)
        top = max(top, vaddr + memsz)
        lines.append("  LOAD %#011x  file %11d  memory %11d (%7.1f MiB)  %s" % (vaddr, filesz, memsz, memsz / (1 << 20), kind))
    lines.append("  mapped at load (flexible memory): %.1f MiB; the image ends at %#x" % (charged / (1 << 20), top))
    dirty = None
    for line in _nm(llvm_elf):
        parts = line.split()
        if len(parts) == 3 and parts[2] == "a64_dirty":
            dirty = int(parts[0], 16)
    if dirty is not None:
        inside = any(v <= dirty < v + m for v, _, m, _ in loads)
        if inside:
            lines.append("  a64_dirty at %#x: in the image's .bss (charged to flexible memory)" % dirty)
        else:
            if dirty < top:
                raise RuntimeError("a64_dirty at %#x lies below the image's end %#x" % (dirty, top))
            if dirty + DIRTY_BYTES >= 1 << 31:
                raise RuntimeError("a64_dirty at %#x: past the code's RIP-relative reach" % dirty)
            lines.append("  a64_dirty at %#x-%#x: past the image (%.1f MiB after its end), backed with direct memory at start" %
                         (dirty, dirty + DIRTY_BYTES, (dirty - top) / (1 << 20)))
    return lines, charged


def link_title(d, kind, app, log_path=None):
    """(ps5) The eboot's link and its title folder, PS5_VulkanTemplate's ps5/tools/link-title.sh's steps (the GPU smoke
    title's, scripts/ps5/build-smoke.sh, run on the console) with this build's archives in <d>:

    1. the CRT (PS5_Vulkan tooling/native/app_crt.cpp), the AGC import stubs and ours of libSceAudioIn (ps5/stubs/: lot 86's
       microphone) (no SDK stub has them);
    2. tools/radv-link.sh's recipe: RADV whole, libc++/libc++abi/libunwind, clang's builtins, the platform layer; the heap
       and thread wraps, the libc names the console lacks bound to ps5_*;
    3. prospero-lld: the CRT; kind "game": libhookshot_game.a whole (the lifted functions are reached through a64_table's
       pointers only), "boot": libhookshot_boot.a whole (scripts/ps5/boot: host/hos_main.c reaches it through a weak
       reference, which pulls nothing in); libhookshot_host.a, libhookshot_rt.a, libhookshot_vk.a whole (one module on
       every other platform: a member dropped silently is what the recipe warns of), libhookshot_eden.a,
       libhookshot_sirit.a, libopus.a; -u mkwvk_init -u vk_icdGetInstanceProcAddr (host/posix/ps5_platform.c's module
       table names them through weak references); --defsym=<name>=0 for the weak references nothing defines; the
       version script, --exclude-libs=ALL, -e _start; the SDK's stubs --as-needed;
    4. the checks: localeconv is the platform's; no import is null at run time (surface() over the ELF: an import that
       only libkernel_sys or libScePosixForWebKit exports, or none);
    5. ps5-native-tool link (the ELF as the console's executable), self --sign to eboot.bin, self --inspect;
    6. the title folder: eboot.bin, sce_sys/param.json (param_json()), sce_module/libc.prx (PS5_Vulkan runtime/, its
       sha256 checked), hookshot-broker.elf (the platform layer's storage broker, a payload: started at stage 5 when the
       data root is a data folder it serves, /data/grapnel (lot 80; /data/hookshot when used in place), unless
       HOOKSHOT_PS5_BROKER=0).

    RADV_ARCHIVE: another RADV (radv_archive()). Writes <d>/link-<kind>/ (the ELF, its map, link.log, surface.txt).
    True when the title folder is made"""
    import hashlib
    import json
    import shutil
    vk, s = vulkan(), sdk()
    archive = radv_archive(kind)
    work = os.path.join(d, "link-" + kind)
    os.makedirs(os.path.join(work, "obj"), exist_ok=True)
    os.makedirs(os.path.join(work, "stubs"), exist_ok=True)
    tool = os.path.join(vk or "", "build", "host", _exe("ps5-native-tool"))
    native = os.path.join(vk or "", "tooling", "native")
    lld = find("lld")
    need = [archive, tool, lld, os.environ["PS5_RECIPE"] if direct() else os.path.join(vk or "", "tools", "radv-link.sh"),
            os.path.join(vk or "", "runtime", "libc.prx")]
    libs = {n: os.path.join(d, n) for n in ("libhookshot_host.a", "libhookshot_rt.a", "libhookshot_vk.a", "libhookshot_eden.a",
                                             "libhookshot_sirit.a", "libopus.a",
                                             "libhookshot_game.a" if kind == "game" else "libhookshot_boot.a")}
    missing = [p for p in need + list(libs.values()) if not os.path.exists(p)]
    if missing:
        print("ps5: the eboot's link: missing " + ", ".join(missing), flush=True)
        return False
    env = dict(os.environ, PS5_PAYLOAD_SDK=s, PS5_CLANG=os.path.join(llvm_bin(), _exe("clang")) if direct() else host_clang())
    log_path = log_path or os.path.join(work, "link.log")
    with open(log_path, "w", encoding="utf-8") as log:
        try:
            crt = os.path.join(work, "obj", "app_crt.o")
            _run(crt_command(["-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-c", os.path.join(native, "app_crt.cpp"),
                              "-o", crt]), log, env=env)
            stubs = []
            # (lot 86: and libSceAudioIn, player 1's DualSense microphone (host/posix/ps5_mic.h): the SDK has no stub of it, ours
            # is in ps5/stubs/, as PS5_RetroArch links it)
            for library, src in (("libSceAgc", os.path.join(vk, "vendor/ps5/sdk/stubs/agc_canary_link_stub.c")),
                                 ("libSceAgcDriver", os.path.join(vk, "vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c")),
                                 ("libSceAudioIn", os.path.join(ROOT, "ps5", "stubs", "libSceAudioIn_link_stub.c"))):
                o = os.path.join(work, "obj", library + "_stub.o")
                so = os.path.join(work, "stubs", library + ".so")
                # (both sides kept: the Windows work's crt_command and command(lld) (the recipe's arguments, no bash), lot 86's
                # sources named whole, its own stub beside the SDK's two)
                _run(crt_command(["-std=c11", "-O2", "-fPIC", "-c", src, "-o", o]), log, env=env)
                _run(command(lld, ["--shared", "-soname", library + ".prx", "-o", so, o]), log, env=env)
                stubs.append(so)
            script, flags, inputs = _recipe_data(work) if direct() else _recipe(vk, s, archive, env)
            script = dirty_script(work, script)   # (a64_dirty out of .bss: host/posix/ps5_early.c backs it)
            sos = sorted(os.path.join(s, "target", "lib", f) for f in os.listdir(os.path.join(s, "target", "lib"))
                         if f.endswith(".so"))
            others = [x for x in inputs if x.endswith(".a") and os.path.isfile(x)] + stubs + sos
            defsyms = ["--defsym=%s=0" % n for n in _weak_undefined(list(libs.values()), others)]
            log.write("weak references set to 0: %s\n" % " ".join(x.split("=")[1] for x in defsyms))
            first = libs["libhookshot_game.a"] if kind == "game" else libs["libhookshot_boot.a"]
            elf, lmap = os.path.join(work, "llvm-pie.elf"), os.path.join(work, "llvm-pie.map")
            # (--threads: lld takes every core otherwise; the build's job count is the machine's share)
            # (--wrap=fopen --wrap=fwrite: stdio's big writes under the data folder, the renderer's caches, made by the storage
            # broker: host/posix/ps5_stdio.c, pulled from libhookshot_host.a by these wraps only)
            # (2026-11-15, host/posix/ps5_compile_heap.c: the driver's compiles in arenas of their own. The heap's allocations
            # (ps5_heap_*, not dlmalloc's mspace_malloc, which its own memalign and realloc call), its segments' map and unmap,
            # RADV's two entry points that compile, and the three calls that make what a compile keeps)
            cmd = command(lld, ["--threads=%d" % link_threads()] + script +
                   ["--eh-frame-hdr"] + flags + ["--wrap=fopen", "--wrap=fwrite", "--wrap=ps5p_heap_map",
                                                 "--wrap=sceKernelAllocateDirectMemory", "--wrap=sceKernelReleaseDirectMemory",
                                                 "--wrap=sceKernelMapDirectMemory", "--wrap=create_mspace"] +
                   ["--wrap=" + n for n in COMPILE_HEAP_WRAPS] + ["--wrap=pthread_" + n for n in (
                       "mutex_init", "mutex_destroy", "cond_init", "cond_destroy", "rwlock_init", "rwlock_destroy")] + defsyms +
                   ["-u", "mkwvk_init", "-u", "vk_icdGetInstanceProcAddr",
                    "--version-script", os.path.join(native, "app-symbols.map"), "--exclude-libs=ALL",
                    "-e", "_start", "-o", elf, "-Map=" + lmap, crt,
                    "--whole-archive", first, "--no-whole-archive",
                    libs["libhookshot_host.a"], libs["libhookshot_rt.a"],
                    "--whole-archive", libs["libhookshot_vk.a"], "--no-whole-archive",
                    libs["libhookshot_eden.a"], libs["libhookshot_sirit.a"], libs["libopus.a"]] +
                   ([dav1d()[1]] if dav1d() else []) +
                   stubs + inputs + ["--as-needed"] + sos)
            with open(os.path.join(work, "link-command.txt"), "w", encoding="utf-8") as f:
                f.write(" ".join(cmd) + "\n")
            t = __import__("time").time()
            _run(cmd, log, env=env)
            print("ps5: %s linked in %.1f s: %s (%.1f MB)" % (kind, __import__("time").time() - t, elf,
                                                             os.path.getsize(elf) / (1 << 20)), flush=True)
            # 4. the checks: localeconv, and every import answered at run time
            plat = os.path.join(s, "target", "lib", "libps5platform.a")
            if " T ps5_localeconv" in "\n".join(_nm(plat, "--defined-only")) and \
                    any(l.split()[-1] == "localeconv" for l in _nm(elf, "--undefined-only") if l.strip()):
                raise RuntimeError("localeconv is not bound to the platform layer's ps5_localeconv")
            rows = surface_elf(elf, stubs)
            with open(os.path.join(work, "surface.txt"), "w", encoding="utf-8") as f:
                for n, (k, where) in sorted(rows.items()):
                    f.write("%-8s %-48s %s\n" % (k, n, where))
            counts = {}
            for k, _ in rows.values():
                counts[k] = counts.get(k, 0) + 1
            bad = sorted(n for n, (k, _) in rows.items() if k in ("null", "missing"))
            print("ps5: %s: the eboot's imports: %s (%s)" % (kind, ", ".join("%s %d" % kv for kv in sorted(counts.items())),
                                                          os.path.join(work, "surface.txt")), flush=True)
            if bad:
                raise RuntimeError("imports null at run time: " + " ".join(bad))
            # 5. the console's executable, signed
            eboot_elf = os.path.join(work, "eboot.elf")
            _run([tool, "link", "--in", elf, "--out", eboot_elf, "--stub-dir", os.path.join(s, "target", "lib")] +
                 [x for so in stubs for x in ("--stub", so)] +
                 ["--module-sdk", "0x02000009", "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"], log, env=env)
            # (what the console's loader maps, and where the write-watch table is: flexible memory is charged at load)
            report, charged = segment_report(eboot_elf, elf)
            log.write("the console executable's segments (%s):\n%s\n" % (eboot_elf, "\n".join(report)))
            print("ps5: %s: the eboot's segments:\n%s\n  param.json kernel.flexibleMemorySize: %d MiB (%.0f MiB left after the "
                  "segments)" % (kind, "\n".join(report), flexible_mb(), flexible_mb() - charged / (1 << 20)), flush=True)
            # 6. the title folder
            if os.path.isdir(app):
                shutil.rmtree(app)
            os.makedirs(os.path.join(app, "sce_sys"))
            os.makedirs(os.path.join(app, "sce_module"))
            _run([tool, "self", "--sign", "--in", eboot_elf, "--out", os.path.join(app, "eboot.bin"), "--magic", "0x1D3D154F"],
                 log, env=env)
            _run([tool, "self", "--inspect", "--file", os.path.join(app, "eboot.bin")], log, env=env)
            write_param_json(os.path.join(app, "sce_sys", "param.json"))
            prx = os.path.join(vk, "runtime", "libc.prx")
            want = open(prx + ".sha256").read().split()[0]
            if hashlib.sha256(open(prx, "rb").read()).hexdigest() != want:
                raise RuntimeError("PS5_Vulkan's runtime/libc.prx does not match its sha256")
            shutil.copyfile(prx, os.path.join(app, "sce_module", "libc.prx"))
            # the storage broker (host/posix/ps5_broker_payload.c, the platform layer's), a payload of its own beside the
            # eboot: host/hos_main.c starts it at stage 5 when the data root is a data folder it serves (as
            # scripts/ps5/build-plat-probe.sh builds it)
            # PS5_MihawkGen's network (third_party/mihawkgen), read from /app0/framegen/network.fgw with RX_VK_MIHAWKGEN=1
            # (gpu/vk/vk_framegen.cpp neural_ready): in the title folder beside the eboot, as the reference ports have it
            network = os.path.join(ROOT, "third_party", "mihawkgen", "framegen", "models", "network.fgw")
            if os.path.exists(network):
                os.makedirs(os.path.join(app, "framegen"), exist_ok=True)
                shutil.copyfile(network, os.path.join(app, "framegen", "network.fgw"))
            broker = os.path.join(ROOT, "host", "posix", "ps5_broker_payload.c")
            if os.path.exists(broker):
                # (grapnel) the same file on every PC: no compiler name written (-fno-ident), the checkout's folder as ".",
                # and the .comment section the SDK's start files and the linker still fill ("Linker: Debian LLD 19.1.7",
                # "Ubuntu clang ...") taken out; its code was already the same from one PC to another. Without bash (direct
                # mode) it is compiled, then linked by lld with what clang's driver gave prospero-lld: the recipe's
                broker_flags = ["-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-fno-ident", "-ffile-prefix-map=%s=." % ROOT,
                                "-I" + os.path.join(ROOT, "host", "posix")]
                elf = os.path.join(app, "hookshot-broker.elf")
                if direct():
                    bo = os.path.join(work, "obj", "hookshot-broker.o")
                    _run(command(find("clang"), broker_flags + ["-c", broker, "-o", bo]), log, env=env)
                    _run(command(lld, fill_places(recipe()["broker_link"], {"obj": bo, "out": elf})), log, env=env)
                else:
                    _run([find("clang")] + broker_flags + ["-o", elf, broker], log, env=env)
                _run([find("llvm-objcopy"), "--remove-section=.comment", elf], log, env=env)
        except RuntimeError as e:
            print("ps5: the eboot's link (%s) failed: %s (%s)" % (kind, e, log_path), flush=True)
            return False
    radv = ""
    prov = os.path.join(os.path.dirname(os.path.dirname(archive)), "PROVENANCE.txt")
    if os.path.exists(prov):
        radv = next((l.split()[-1][:11] for l in open(prov) if l.startswith("revision:")), "")
    with open(os.path.join(work, "title.json"), "w") as f:
        json.dump({"kind": kind, "title": app, "eboot_bytes": os.path.getsize(os.path.join(app, "eboot.bin")),
                   "radv_archive": archive, "radv_revision": radv}, f, indent=2)
    print("ps5: ==> %s (%s): %s, eboot.bin %d bytes; RADV %s (%s)" % (TITLE_ID, kind, app,
                                                                   os.path.getsize(os.path.join(app, "eboot.bin")),
                                                                   radv or "?", archive), flush=True)
    return True


# ---- the runtime surface (PS5_VulkanTemplate skills/ps5-homebrew/references/runtime-surface.md) ----------------------------
# A name the SDK's stubs define links, and may still be NULL on the console. What our archives import, by who answers it:
#   platform  the platform layer has ps5_<name> and PS5_Vulkan's link recipe binds or wraps <name> to it: covered
#   platform? the platform layer has ps5_<name>, the recipe does not bind it: the console's own <name> is called
#             (bind it in the recipe, or call ps5_<name>)
#   module    a module a title loads exports it (libkernel_web, libSceLibcInternal, libScePad...): unproven until a run calls it
#   null      only libkernel_sys or libScePosixForWebKit define it: NULL at run time (link-title.sh refuses these)
#   missing   nothing defines it: the link fails
#   ours      another of our archives defines it (the game, the renderer, RADV)
#   runtime   the recipe's libc++, libc++abi, libunwind or clang's builtins (__emutls_get_address) define it
NULL_MODULES = ("libkernel_sys.so", "libScePosixForWebKit.so")


def _nm(path, *opts):
    r = subprocess.run([find("nm"), *opts, path], capture_output=True, text=True)
    return r.stdout.splitlines()


def _defined(path, dynamic):
    out = set()
    for line in _nm(path, *(["-D"] if dynamic else []), "--defined-only"):
        parts = line.split()
        if len(parts) >= 3 and parts[1] not in ("U", "w", "v"):
            out.add(parts[-1].split("@")[0])
    return out


def surface(archives, others=()):
    """{name: (class, where)} for every symbol the archives use and do not define; others: archives that may answer"""
    undefined, ours = set(), set()
    for a in archives:
        for line in _nm(a, "--undefined-only"):
            parts = line.split()
            if parts and parts[-1] and not parts[-1].endswith(":"):
                undefined.add(parts[-1])
        ours |= _defined(a, False)
    other = set()
    for a in others:
        if os.path.exists(a):
            other |= _defined(a, False)
    lib = os.path.join(sdk(), "target", "lib")
    modules = {}
    for f in sorted(os.listdir(lib)):
        if f.endswith(".so"):
            for n in _defined(os.path.join(lib, f), True):
                modules.setdefault(n, []).append(f)
    platform_lib = _defined(os.path.join(lib, "libps5platform.a"), False)
    platform = {n[4:] for n in platform_lib if n.startswith("ps5_")}
    # (the link recipe's other archives: libc++, libc++abi, libunwind, and clang's builtins for __emutls_get_address)
    runtime = set()
    for a in ("libc++.a", "libc++abi.a", "libunwind.a"):
        if os.path.exists(os.path.join(lib, a)):
            runtime |= _defined(os.path.join(lib, a), False)
    try:
        res = subprocess.run([os.environ.get("PS5_CLANG") or "clang", "--print-resource-dir"], capture_output=True,
                             text=True).stdout.strip()
        builtins = os.path.join(res, "lib", "linux", "libclang_rt.builtins-x86_64.a")
        if os.path.exists(builtins):
            runtime |= _defined(builtins, False)
    except OSError:
        pass
    recipe = ""
    if vulkan() and os.path.exists(os.path.join(vulkan(), "tools", "radv-link.sh")):
        recipe = open(os.path.join(vulkan(), "tools", "radv-link.sh")).read()
    import re
    bound = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", recipe))
    out = {}
    for n in sorted(undefined - ours):
        if n.startswith("__real_") or n.startswith("__wrap_"):
            continue
        if n in platform and n in bound:
            out[n] = ("platform", "ps5_" + n)
        elif n in platform_lib:
            out[n] = ("platform", "libps5platform.a")
        elif n in runtime:
            out[n] = ("runtime", "libc++ / libunwind / clang builtins")
        elif n in other:
            out[n] = ("ours", "")
        elif n in modules and any(m not in NULL_MODULES for m in modules[n]):
            out[n] = ("platform?" if n in platform else "module", " ".join(m[:-3] for m in modules[n] if m not in NULL_MODULES))
        elif n in modules:
            out[n] = ("null", " ".join(m[:-3] for m in modules[n]))
        elif n in platform:
            out[n] = ("platform?", "ps5_" + n + " (not bound by the recipe)")
        else:
            out[n] = ("missing", "")
    return out


def surface_elf(elf, stubs=()):
    """{import: (class, module)} for a linked eboot (its ELF before the native tool, llvm-pie.elf): every dynamic import, by
    the module a title loads that exports it ("module"), or "null" when only libkernel_sys or libScePosixForWebKit export it
    (NULL at run time: its first call jumps to 0), or "missing" when none does. stubs: the AGC link stubs the eboot named"""
    lib = os.path.join(sdk(), "target", "lib")
    modules = {}
    for f in sorted(os.listdir(lib)):
        if f.endswith(".so"):
            for n in _defined(os.path.join(lib, f), True):
                modules.setdefault(n, []).append(f)
    for so in stubs:
        for n in _defined(so, True):
            modules.setdefault(n, []).append(os.path.basename(so))
    out = {}
    for line in _nm(elf, "-D", "--undefined-only"):
        parts = line.split()
        if not parts or parts[0] not in ("U", "w", "v"):
            continue
        n = parts[-1].split("@")[0]
        mods = modules.get(n, [])
        good = [m[:-3] for m in mods if m not in NULL_MODULES]
        out[n] = ("module", " ".join(good)) if good else ("null", " ".join(m[:-3] for m in mods)) if mods else ("missing", "")
    return out


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 2 and sys.argv[1] == "surface-elf":
        # python -I nx2/toolchain_ps5.py surface-elf <llvm-pie.elf> [<AGC stub .so>...]: the eboot's imports, none null
        rows = surface_elf(sys.argv[2], sys.argv[3:])
        counts = {}
        for n, (k, where) in sorted(rows.items()):
            counts[k] = counts.get(k, 0) + 1
            print("%-8s %-48s %s" % (k, n, where))
        print("# " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
        sys.exit(1 if counts.get("null") or counts.get("missing") else 0)
    if len(sys.argv) > 2 and sys.argv[1] == "surface":
        # python -I nx2/toolchain_ps5.py surface <archive>... [--others <archive>...]
        args = sys.argv[2:]
        others = args[args.index("--others") + 1:] if "--others" in args else []
        mine = args[:args.index("--others")] if "--others" in args else args
        rows = surface(mine, others)
        counts = {}
        for n, (k, where) in rows.items():
            counts[k] = counts.get(k, 0) + 1
            print("%-10s %-40s %s" % (k, n, where))
        print("# " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    else:
        print("usage: toolchain_ps5.py surface <archive>... [--others <archive>...] | surface-elf <llvm-pie.elf> [<stub.so>...]")
