#!/usr/bin/env bash
# Hookshot on the PS5: the Vulkan back end's smoke title, PPSA99144 "Hookshot Vulkan Smoke" (scripts/ps5/smoke/vk_smoke.cpp),
# linked and laid out as a title folder ready to upload: dist/PPSA99144/{eboot.bin, sce_sys/param.json, sce_module/libc.prx}.
#
#   scripts/ps5/build-smoke.sh        (builds the archives first: scripts/ps5/build-gpu.sh smoke)
#
# The link is the one the phase 2 probe (PPSA99141) ran on the console with: probe/ps5/link-probe.sh, after
# PS5_VulkanTemplate's ps5/tools/link-title.sh (MIT, Mihawk). PS5_Vulkan's RADV release archive and link recipe
# (tools/radv-link.sh: RADV whole, the platform layer, the heap and thread wraps, libc names bound to ps5_*), its CRT,
# the AGC link stubs, its native tool (ELF -> fake SELF, signed) and libc.prx. The payload SDK is the template's pin.
#
# On the console: the folder goes to /data/homebrew/PPSA99144/ (ShadowMount+ registers it); the title writes its results
# to /data/homebrew/PPSA99144/vk-smoke.txt and klog (prefix [hookshot-vk-smoke]), then closes itself through the shell.
#
#   RADV_ARCHIVE   another libvulkan_radeon.ps5.a (default: PS5_Vulkan's release archive)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ -n ${LINK_TRACE:-} ]] && set -x
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
stack=$(cd -- "${PS5_STACK:-$root/../../ps5-stack}" && pwd)
sdk=${PS5_PAYLOAD_SDK:-$stack/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk}
vulkan=$stack/PS5_Vulkan
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang)}
out=$root/out-ps5
work=$out/smoke
param=$root/scripts/ps5/smoke/sce_sys/param.json
tool=$vulkan/build/host/ps5-native-tool
native=$vulkan/tooling/native

for f in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" "$vulkan/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing: $f" >&2; exit 2; }
done
bash "$root/scripts/ps5/build-gpu.sh" smoke
mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
stub() {   # (RADV calls AGC, which the SDK has no link stubs for)
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

objects=("$work/obj/app_crt.o" "$out/obj/smoke/vk_smoke.cpp.o" "$out/obj/smoke/vk_census.cpp.o"
    "$out/obj/smoke/win32_smoke_ps5.c.o")
# (Hookshot's archives: the back end, Eden's recompiler, sirit, and the menu's GDI. Whole: nothing in them is reached only
# through a weak reference or a constructor today, but the back end is one module on every other platform, and a
# member dropped silently is the failure the link recipe warns about)
hookshot=(--whole-archive "$out/lib/libhookshot_vk.a" --no-whole-archive
    "$out/lib/libhookshot_eden.a" "$out/lib/libhookshot_sirit.a" "$out/lib/libhookshot_vk_gdi.a")

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# (weak references to TLS init functions that nothing defines: 0, as the Windows and Linux links leave them. Clang calls a
# thread_local's init function (_ZTH*) through a weak reference when another file defines the variable; Hookshot's are
# constant-initialized (vk_shader.cpp's extern view of Eden's g_pointer_diag, g_nvn2_compute_*..., vk_state.h's
# seed_cs_control), so none exists. In a PIE, lld 19 makes such a reference an import (it has no -z
# nodynamic-undefined-weak yet), and the native tool refuses it: "no public SDK stub exports required symbol")
nm=$sdk/bin/llvm-nm
tls_defsyms=()
for name in $(comm -23 <("$nm" "${objects[@]}" "$out"/lib/libhookshot_*.a 2>/dev/null | awk '$1 == "w" && $2 ~ /^_ZTH/ { print $2 }' | sort -u) \
                       <("$nm" --defined-only "${objects[@]}" "$out"/lib/libhookshot_*.a 2>/dev/null | awk '$3 ~ /^_ZTH/ { print $3 }' | sort -u)); do
    tls_defsyms+=("--defsym=$name=0")
done
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" "${tls_defsyms[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" -Map="$work/llvm-pie.map" \
    "${objects[@]}" "${hookshot[@]}" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
# (the probe's checks) localeconv must be the platform layer's: the console's gives no decimal point
if grep -q " T ps5_localeconv$" <<<"$("$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null)" &&
        grep -q " U localeconv$" <<<"$("$sdk/bin/llvm-nm" "$work/llvm-pie.elf" 2>/dev/null)"; then
    echo "build-smoke.sh: localeconv is not bound to the platform layer's ps5_localeconv" >&2
    exit 1
fi
# an import no module a title loads exports is null at run time, and its first call jumps to 0
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "build-smoke.sh: imports no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    exit 1
fi
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf.new" \
    --stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app=$root/dist/$title_id
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
[[ -f $root/dist/.gitignore ]] || echo '*' > "$root/dist/.gitignore"
"$tool" self --sign --in "$work/eboot.elf.new" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
mv "$work/eboot.elf.new" "$work/eboot.elf"
radv=$(sed -n 's/^revision: //p' "$(dirname "$(dirname "$archive")")/PROVENANCE.txt" 2>/dev/null || true)
printf '==> %s: %s (eboot.bin %s bytes; RADV %s; gpu/vk %s)\n' "$title_id" "$app" "$(stat -c %s "$app/eboot.bin")" \
    "${radv:0:11}" "$(git -C "$root" describe --always --dirty --abbrev=10 2>/dev/null || echo unknown)"
