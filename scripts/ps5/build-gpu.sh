#!/usr/bin/env bash
# Hookshot on the PS5: the Vulkan back end as static archives for the console (phase 3, the GPU side).
#
# On Windows and Linux nx2/build.py links gpu/vk into hookshot_vk.dll / hookshot_vk.so, which host/rx_backend.c loads
# at run time. A PS5 title cannot load a module it brought (dlopen and sceKernelLoadStartModule are refused:
# PS5_VulkanTemplate skills/ps5-porting/references/loading-code.md), so on the console the same objects go into static
# archives that the title's link takes whole:
#
#   out-ps5/lib/libhookshot_vk.a        gpu/vk/*.cpp and suyu's texture decoders (vk_sources() in nx2/build.py)
#   out-ps5/lib/libhookshot_eden.a      gpu/vk/eden: Eden's shader recompiler with its SPIR-V back end
#   out-ps5/lib/libhookshot_sirit.a     gpu/vk/sirit: sirit with Eden's mesh shader ops
#   out-ps5/lib/libhookshot_vk_gdi.a    host/posix/gdi_posix.c, unchanged: the bit of GDI the Insert menu draws text
#                                       with (the copy hookshot_vk.so carries on Linux). For the smoke title only: the
#                                       game's eboot gets it once, from host/ (one copy per binary). host/posix/
#                                       win32_posix.c, the other file of that copy, builds for the console on branch
#                                       ps5-host only (here, at 8b2e1d3: Linux futexes, gettid, /proc): the smoke title
#                                       has its own, scripts/ps5/smoke/win32_smoke_ps5.c
#
# Target "smoke": the smoke title's objects too (scripts/ps5/smoke; scripts/ps5/build-smoke.sh links and packages them).
#
# The flags are build.py's for the Vulkan back end (vflags) with RX_TARGET=ps5 (branch ps5-host, nx2/toolchain_ps5.py): the
# payload SDK's prospero-clang++ (x86_64-sie-ps5, its libc and libc++ 18 headers, emulated TLS, IEEE denormals, RTTI and
# exceptions), -march=x86-64-v3 -mtune=znver2 (the console's Zen 2), frame pointers and sections. With that branch merged,
# RX_TARGET=ps5 RX_VK_ONLY=1 RX_CPM=<this worktree>/third_party-ps5/cpm python -I nx2/build.py --standalone builds the
# same objects into one libhookshot_vk.a; this script stays for the smoke title and the archives on their own.
#
#   scripts/ps5/build-gpu.sh [ninja target...]     JOBS=3 (default), nice 10
#
#   PS5_STACK         the ps5-stack folder (default: ../../ps5-stack beside this worktree's hookshot-ps5/wt)
#   PS5_PAYLOAD_SDK   the payload SDK (default: PS5_VulkanTemplate's pinned one, .deps/native/ps5-payload-sdk)
#   RX_CPM            Vulkan-Headers, SPIRV-Headers and VMA in CPM's layout (default: third_party-ps5/cpm, fetched by
#                     scripts/ps5/fetch-headers.sh at the revisions scripts/linux/deps.sh pins)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
stack=$(cd -- "${PS5_STACK:-$root/../../ps5-stack}" && pwd)
sdk=${PS5_PAYLOAD_SDK:-$stack/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk}
vulkan=$stack/PS5_Vulkan
out=$root/out-ps5
jobs=${JOBS:-3}
cpm=${RX_CPM:-$root/third_party-ps5/cpm}
[[ -d $cpm/vulkanheaders ]] || bash "$root/scripts/ps5/fetch-headers.sh" >&2
newest() { local d=$1; ls -1 "$d" | sort | tail -1 | sed "s|^|$d/|"; }   # (cpm_package(): the newest version)
vk_headers=$(newest "$cpm/vulkanheaders")
spirv_headers=$(newest "$cpm/spirv-headers")
vma=$(newest "$cpm/vulkanmemoryallocator")
ninja=$(command -v ninja || echo "$stack/.venv/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang)}
for f in "$sdk/bin/llvm-ar" "$sdk/bin/prospero-clang++" "$sdk/bin/prospero-clang" "$ninja" "$PS5_CLANG" "$vk_headers/include/vulkan/vulkan.h" \
        "$spirv_headers/include/spirv/unified1/spirv.hpp11" "$vma/include/vk_mem_alloc.h"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 2; }
done
mkdir -p "$out"
[[ -f $out/.gitignore ]] || echo '*' > "$out/.gitignore"

gpu=$root/gpu
vk=$gpu/vk
posix=$root/host/posix
# (build.py's shader stamp: what translates shaders, so the program caches follow the translator, not the whole back end)
shader_stamp=$(python3 -I - "$vk" <<'EOF'
import hashlib, os, sys
vk = sys.argv[1]
h = hashlib.sha1()
files = [os.path.join(d, f) for top in ("eden", "sirit", "compat") for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
for f in sorted(files) + [os.path.join(vk, n) for n in ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h")]:
    h.update(os.path.relpath(f, vk).replace("\\", "/").encode() + open(f, "rb").read().replace(b"\r\n", b"\n"))
print(h.hexdigest()[:16])
EOF
)
version=$(cd "$root" && python3 -I nx2/version.py)
# (the smoke title's census: every Vulkan format vk_formats.cpp maps an NVN format to)
mkdir -p "$out/gen"
grep -o 'VK_FORMAT_[A-Za-z0-9_]*' "$vk/vk_formats.cpp" | grep -Ev '^VK_FORMAT_(UNDEFINED|FEATURE_.*)$' | LC_ALL=C sort -u |
    sed 's/.*/X(&)/' > "$out/gen/hookshot_formats.inc.tmp"
cmp -s "$out/gen/hookshot_formats.inc.tmp" "$out/gen/hookshot_formats.inc" 2>/dev/null &&
    rm "$out/gen/hookshot_formats.inc.tmp" || mv "$out/gen/hookshot_formats.inc.tmp" "$out/gen/hookshot_formats.inc"

# --- the flags ---
compile_cxx=$sdk/bin/prospero-clang++
compile_cc=$sdk/bin/prospero-clang
common=(-march=x86-64-v3 -mtune=znver2 -ffp-contract=off -O2 -g0 -DNDEBUG -Wno-everything
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections -I"$posix" -D_GNU_SOURCE)
# (the PS5 target's clang defaults: no RTTI and no exceptions; Eden's recompiler throws, gpu/vk catches)
cxx=("${common[@]}" -std=c++20 -fexceptions -fcxx-exceptions -frtti
    -include algorithm -include cstdio -include cstdlib -include cstring -include functional -include string -include vector
    -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2
    -I"$vk/compat" -I"$gpu/compat" -I"$vk/eden" -I"$gpu/third_party/suyu"
    -I"$vk/sirit/include" -I"$vk/sirit/src"
    -I"$spirv_headers/include" -I"$spirv_headers/include/spirv/unified1"
    -I"$vk_headers/include" -I"$vk" -I"$vma/include")
cc=("${common[@]}" -std=gnu11 -I"$root/third_party/stb")

q() { printf '%q ' "$@"; }   # (one ninja variable, shell-quoted)
esc() { sed 's/\$/$$/g; s/ /$ /g; s/:/$:/g' <<<"$1"; }

{
    echo "# generated by scripts/ps5/build-gpu.sh"
    echo "ninja_required_version = 1.10"
    echo "sdkenv = PS5_PAYLOAD_SDK=$(printf '%q' "$sdk") PS5_CLANG=$(printf '%q' "$PS5_CLANG")"
    echo "compile_cxx = $(printf '%q' "$compile_cxx")"
    echo "compile_cc = $(printf '%q' "$compile_cc")"
    echo "cxxflags = $(q "${cxx[@]}")"
    echo "cflags = $(q "${cc[@]}")"
    echo "ar = $(printf '%q' "$sdk/bin/llvm-ar")"
    echo "rule cxx"
    echo "  command = env \$sdkenv \$compile_cxx \$cxxflags \$extra -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "  description = c++ \$in"
    echo "rule cc"
    echo "  command = env \$sdkenv \$compile_cc \$cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "  description = cc \$in"
    echo "rule ar"
    echo "  command = rm -f \$out && \$ar rcsD \$out @\$out.rsp"
    echo "  rspfile = \$out.rsp"
    echo "  rspfile_content = \$in"
    echo "  description = ar \$out"
    lib() {   # name rule extra-flags sources...
        local name=$1 rule=$2 extra=$3
        shift 3
        local objs=() src rel obj
        for src in "$@"; do
            rel=${src#"$root/"}
            obj=$out/obj/$name/${rel//\//_}.o
            objs+=("$(esc "$obj")")
            echo "build $(esc "$obj"): $rule $(esc "$src")"
            case $src in
                */vk_shader.cpp) echo "  extra = -DHOOKSHOT_SHADER_STAMP=0x${shader_stamp}ull" ;;
                */vk_menu.cpp) echo "  extra = $(printf '%q' "-DHOOKSHOT_VERSION=\"$version\"")" ;;
                *) [[ -n $extra ]] && echo "  extra = $extra" ;;
            esac
        done
        echo "build $(esc "$out/lib/lib$name.a"): ar ${objs[*]}"
        echo "default $(esc "$out/lib/lib$name.a")"
    }
    mapfile -t eden < <(find "$vk/eden" -name '*.cpp' | LC_ALL=C sort)
    mapfile -t sirit < <(find "$vk/sirit/src" -name '*.cpp' | LC_ALL=C sort)
    mapfile -t back < <(find "$vk" -maxdepth 1 -name '*.cpp' | LC_ALL=C sort)
    back+=("$gpu/third_party/suyu/video_core/textures/decoders.cpp" "$gpu/third_party/suyu/video_core/textures/astc.cpp")
    lib hookshot_eden cxx "" "${eden[@]}"
    lib hookshot_sirit cxx "" "${sirit[@]}"
    lib hookshot_vk cxx "" "${back[@]}"
    lib hookshot_vk_gdi cc "" "$posix/gdi_posix.c"
    # (the smoke title: its objects, with the back end's flags, so that it sees gpu/vk's structures as gpu/vk does)
    smoke=$root/scripts/ps5/smoke
    sobj=()
    for src in "$smoke/vk_smoke.cpp" "$smoke/vk_census.cpp" "$smoke/win32_smoke_ps5.c"; do
        obj=$out/obj/smoke/$(basename "$src").o
        sobj+=("$(esc "$obj")")
        case $src in
            *.c) echo "build $(esc "$obj"): cc $(esc "$src")" ;;
            *) echo "build $(esc "$obj"): cxx $(esc "$src") | $(esc "$out/gen/hookshot_formats.inc")"
               echo "  extra = -I$(esc "$out/gen") -I$(esc "$smoke")" ;;
        esac
    done
    echo "build smoke: phony ${sobj[*]} $(esc "$out/lib/libhookshot_vk.a") $(esc "$out/lib/libhookshot_eden.a") $(esc "$out/lib/libhookshot_sirit.a") $(esc "$out/lib/libhookshot_vk_gdi.a")"
} > "$out/build.ninja.tmp"
mv "$out/build.ninja.tmp" "$out/build.ninja"
exec nice -n 10 "$ninja" -C "$out" -j "$jobs" -k 0 "$@"
