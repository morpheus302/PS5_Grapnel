#!/usr/bin/env bash
# Hookshot on the PS5: the platform probe title, PPSA99147 "Hookshot Platform Probe" (scripts/ps5/plat-probe), and the
# storage broker payload it starts (host/posix/ps5_broker_payload.c), laid out as a title folder ready to upload:
# dist/PPSA99147/{eboot.bin, sce_sys/param.json, sce_module/libc.prx, hookshot-broker.elf, plat-probe.ini, EXPECTED.txt,
# nup-hashes.txt}, copied to <hookshot-ps5>/probe/dist/plat-probe/PPSA99147.
# nup-hashes.txt (the nup check's list, game-derived: it never leaves hookshot-ps5/) is NUP_HASHES, by default
# <hookshot-ps5>/probe/private/nup-hashes.txt (scripts/ps5/nup/check.sh hashlist); without it the nup check says so.
#
#   scripts/ps5/build-plat-probe.sh             the title (builds the runtime's objects first: nx2/build.py --runtime)
#   scripts/ps5/build-plat-probe.sh host-test   the window's code on this PC, against PS5_PayloadSDK's kernel model
#
# The probe is the platform layer's own objects as nx2/build.py compiles them for the eboot (RX_TARGET=ps5): host/posix's
# ps5_platform, ps5_window, ps5_files, ps5_broker_client, hos_sdl_ps5, win32_posix; host/'s mkw_glue and pad_sdl; a64-kit's
# runtime (its 128 MiB write-watch table). The link is scripts/ps5/build-smoke.sh's on branch ps5-gpu (after
# PS5_VulkanTemplate's ps5/tools/link-title.sh, MIT): PS5_Vulkan's RADV release archive and link recipe, its CRT, the AGC
# link stubs, its native tool (ELF -> fake SELF, signed) and libc.prx.
#
#   JOBS (3), PYTHON (the checkout's .venv), RADV_ARCHIVE, PS5_STACK, PS5_PAYLOAD_SDK, NUP_HASHES, PLAT_PROBE_OUT
#   HOST_TEST_BIN: where host-test puts its program (two checks that run it at once each name their own)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
stack=$(cd -- "${PS5_STACK:-$root/../../ps5-stack}" && pwd)
sdk=${PS5_PAYLOAD_SDK:-$stack/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk}
platform=$stack/PS5_PayloadSDK/platform
vulkan=$stack/PS5_Vulkan
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
probe=$root/scripts/ps5/plat-probe
out=${PLAT_PROBE_OUT:-$root/out-ps5/plat-probe}   # (PLAT_PROBE_OUT: another folder, for two host-tests at once: tests/run_all.py)
python=${PYTHON:-$root/../../.venv/bin/python}
[[ -x $python ]] || python=python3
mkdir -p "$out/obj" "$root/out-ps5"
[[ -f $root/out-ps5/.gitignore ]] || echo '*' > "$root/out-ps5/.gitignore"

if [[ ${1:-} == host-test ]]; then
    # (the window's bookkeeping, the signal-safe fault path and the stepped commits on the PC: ps5_window.c over the
    # platform's model of the console's kernel, direct memory a memfd; the console's own behaviour is the title's to prove)
    cc=${CC:-cc}
    "$cc" -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -DHOOKSHOT_PS5_HOST_MODEL -D_GNU_SOURCE \
        -I"$root/host/posix" -I"$platform/include" -I"$platform/src" -I"$platform/tests" \
        "$root/host/posix/ps5_window.c" "$root/host/posix/ps5_fiber.c" "$probe/host_window_test.c" "$platform/src/shm.c" "$platform/src/placement.c" \
        "$platform/tests/host_kernel.c" -Wl,--wrap=sceKernelQueryMemoryProtection -o "${HOST_TEST_BIN:-$out/host_window_test}"
    exec "${HOST_TEST_BIN:-$out/host_window_test}"
fi

for f in "$archive" "$vulkan/build/host/ps5-native-tool" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" "$vulkan/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 2; }
done
command -v glslangValidator >/dev/null || { echo "missing: glslangValidator (the probe's compute shader)" >&2; exit 2; }

# 1. the runtime's objects, as the eboot gets them
(cd "$root" && RX_TARGET=ps5 nice -n 19 "$python" -I nx2/build.py --runtime --jobs "${JOBS:-3}" > "$out/runtime-build.log" 2>&1) || {
    tail -40 "$out/runtime-build.log" >&2; exit 1; }
objdir=$root/work/sa/build-ps5/obj
host_objects=()
for o in ps5_platform ps5_window ps5_fiber ps5_files ps5_stdio ps5_broker_client hos_sdl_ps5 win32_posix mkw_glue pad_sdl nup_fs; do
    host_objects+=("$objdir/host_$o.o")
done
host_objects+=("$objdir/a64_rt.o")

# 2. the compute shader, as words
glslangValidator -V --target-env vulkan1.1 -o "$out/probe_import.spv" "$probe/probe_import.comp" > /dev/null
"$python" -I - "$out/probe_import.spv" "$out/probe_import_spv.h" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read()
words = struct.unpack("<%dI" % (len(data) // 4), data)
with open(sys.argv[2], "w") as f:
    f.write("/* probe_import.comp, SPIR-V (build-plat-probe.sh) */\nstatic const uint32_t probe_import_spv[] = {\n")
    for i in range(0, len(words), 8):
        f.write("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",\n")
    f.write("};\n")
EOF

# 3. the probe's own sources, with the runtime's flags (nx2/toolchain_ps5.py)
revision=$(git -C "$root" describe --always --dirty --abbrev=10 2>/dev/null || echo unknown)
cflags=(-DA64_HOST_BASE=0x8000000000ULL -D_GNU_SOURCE -march=x86-64-v3 -mtune=znver2 -fno-omit-frame-pointer -O2 -g0
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror=implicit-function-declaration
    "-DPROBE_REVISION=\"$revision\"" -I"$root/host/posix" -I"$root/host" -I"$probe" -I"$out"
    -I"$(dirname "$(dirname "$archive")")/include")
for s in plat_probe probe_vk probe_runtime probe_nup probe_sched; do
    "$sdk/bin/prospero-clang" "${cflags[@]}" -c "$probe/$s.c" -o "$out/obj/$s.o"
done
cc18() { PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=${PS5_CLANG:-$(command -v clang)} sh "$vulkan/tooling/prospero-clang18" "$@"; }
cc18 -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$vulkan/tooling/native/app_crt.cpp" -o "$out/obj/app_crt.o"

# 4. the storage broker, a payload of its own (the payload SDK, as Forever's writebroker Makefile builds its)
"$sdk/bin/prospero-clang" -O2 -Wall -Wextra -Werror -pthread -I"$root/host/posix" -o "$out/hookshot-broker.elf" \
    "$root/host/posix/ps5_broker_payload.c"

# 5. the eboot (build-smoke.sh's link)
mkdir -p "$out/stubs"
stub() {   # (RADV calls AGC, which the SDK has no link stubs for)
    local library=$1 source=$2
    cc18 -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$out/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" -o "$out/stubs/${library}.so" "$out/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
objects=("$out/obj/app_crt.o" "$out/obj/plat_probe.o" "$out/obj/probe_vk.o" "$out/obj/probe_runtime.o" "$out/obj/probe_nup.o"
    "$out/obj/probe_sched.o" "${host_objects[@]}")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang)}
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# (the weak references to what the probe does not link: ps5_platform.c's mkwvk_init (the renderer), win32_posix.c's
# hos_posix_keys, hos_posix_window and hos_posix_post_message (display_sdl.c's window). A weak undefined symbol would be an
# import the native tool refuses, as build-smoke.sh's _ZTH* are: 0, as the Linux link leaves them. Never the linker's own
# __init_array_start and its kind)
nm=$sdk/bin/llvm-nm
weak_defsyms=()
for name in $(comm -23 <("$nm" "${objects[@]}" 2>/dev/null | awk '$1 == "w" && $2 !~ /^__/ { print $2 }' | sort -u) \
                       <("$nm" --defined-only "${objects[@]}" "$archive" "$sdk/target/lib/libps5platform.a" 2>/dev/null | awk 'NF == 3 { print $3 }' | sort -u)); do
    weak_defsyms+=("--defsym=$name=0")
done
echo "build-plat-probe.sh: weak references left at 0: ${weak_defsyms[*]#--defsym=}" > "$out/weak.txt"
# (--wrap=fopen --wrap=fwrite: stdio's big writes under /data/hookshot through the storage broker, host/posix/ps5_stdio.c, as
# the eboot's link has them)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" --wrap=fopen --wrap=fwrite "${weak_defsyms[@]}" \
    --version-script "$vulkan/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$out/llvm-pie.elf" -Map="$out/llvm-pie.map" \
    "${objects[@]}" "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
# an import no module a title loads exports is null at run time, and its first call jumps to 0
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$out/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "build-plat-probe.sh: imports no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    exit 1
fi
tool=$vulkan/build/host/ps5-native-tool
"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" --stub-dir "$sdk/target/lib" --stub "$out/stubs/libSceAgc.so" \
    --stub "$out/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

# 6. the title folder
app=$root/dist/PPSA99147
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
[[ -f $root/dist/.gitignore ]] || echo '*' > "$root/dist/.gitignore"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$probe/sce_sys/param.json" "$app/sce_sys/param.json"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
cp "$out/hookshot-broker.elf" "$probe/plat-probe.ini" "$probe/EXPECTED.txt" "$app/"
nup_hashes=${NUP_HASHES:-$root/../../probe/private/nup-hashes.txt}
if [[ -f $nup_hashes ]]; then cp "$nup_hashes" "$app/nup-hashes.txt"; else echo "build-plat-probe.sh: no $nup_hashes: the nup check will say so" >&2; fi
radv=$(sed -n 's/^revision: //p' "$(dirname "$(dirname "$archive")")/PROVENANCE.txt" 2>/dev/null || true)
(cd "$app" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
shared=$root/../../probe/dist/plat-probe
mkdir -p "$shared"
rm -rf "$shared/PPSA99147"
cp -r "$app" "$shared/PPSA99147"
printf '==> PPSA99147: %s (eboot.bin %s bytes; RADV %s; ps5-plat %s); also %s\n' "$app" "$(stat -c %s "$app/eboot.bin")" "${radv:0:11}" \
    "$revision" "$(cd "$shared/PPSA99147" && pwd)"
