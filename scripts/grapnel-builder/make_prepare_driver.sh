#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The driver of the shaders' preparation (the toolchain's prepare/, make_prepare_tool.py --mesa-build): the console's RADV,
# PS5_Mesa with Grapnel's patches, built for a PC on its PS5 winsys's host model (radv_ps5_platform.c's PC branch: the
# console's memory map modelled, nothing run on a GPU), so that the shader cache it fills has the console driver's keys and
# bytes. Its shader code must be the console's to the byte: the same compiler (clang 19), the same C++ library headers for
# ACO (the payload SDK's libc++ 18.1), x86-64-v3, and the console build's ID.
#
#   make_prepare_driver.sh --system linux-x64|windows-x64 --mesa <PS5_Mesa source> --build <folder>
#       --sdk-cxx <the payload SDK's target/include/c++/v1> [--libcxx <Debian's libc++ 19 runtime, linux-x64>]
#       [--mingw <llvm-mingw 20250114 for Linux, windows-x64>] [--clc-bin <mesa_clc and vtn_bindgen2>] [--jobs N] [--lock F]
#   -> linux-x64:   <build>/src/amd/vulkan/libvulkan_radeon.so (+ the libdrm subproject's libdrm.so.2)
#      windows-x64: <build>/src/amd/vulkan/vulkan_radeon.dll (zlib and winpthreads inside)
#
# <PS5_Mesa source>: github.com/mihawk-99/PS5_Mesa at 0f86253f674c425052948eb3cb116c81e646a734 with ps5/mesa-patches/0001 to
# 0007 applied, and for windows-x64 ps5/mesa-patches/windows/0001 to 0004 on top, and for both ps5/mesa-patches/pc/0001
# (since Grapnel 0.2.4: CLFLUSH off the console, for CPUs without CLFLUSHOPT) last (ps5/mesa-patches/README.md).
# <clc-bin>: mesa_clc and vtn_bindgen2 built for this PC from the same PS5_Mesa revision (PS5_Vulkan's tools/build-radv.sh
# makes them first: -Dmesa-clc=system).
# Linux runs the driver with Debian's libc++ 19 (libc++1-19, libc++abi1-19, unpacked: --libcxx), the SDK's 18.1 headers
# compiling it, as the console's archive; Windows links llvm-mingw's libc++ 19 in. Both clang 19.1.7.
#   -mno-ms-bitfields (Windows): MinGW lays bitfields out as MSVC does by default; RADV hashes and stores structs with
#   bitfields, and the cache differed from the console's (18 % of its keys in common) until the layout was SysV's.
set -euo pipefail
jobs=$(nproc) lock= libcxx= mingw= clc=
while [ $# -gt 0 ]; do
    case $1 in
        --system) system=$2; shift 2 ;;
        --mesa) mesa=$2; shift 2 ;;
        --build) build=$2; shift 2 ;;
        --sdk-cxx) sdk_cxx=$2; shift 2 ;;
        --libcxx) libcxx=$2; shift 2 ;;
        --mingw) mingw=$2; shift 2 ;;
        --clc-bin) clc=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --lock) lock=$2; shift 2 ;;
        *) echo "make_prepare_driver.sh: unknown $1" >&2; exit 2 ;;
    esac
done
build_id=1599cbb5b1a0de5dc9beb4d7026883a49e9b5b63   # (the console's archive's: its shader cache's key, ps5/mesa-patches/README.md)
grep -q "define _LIBCPP_VERSION 180100" "$sdk_cxx/__config" || { echo "no libc++ 18.1 headers in $sdk_cxx" >&2; exit 2; }
[ -n "$clc" ] && export PATH=$clc:$PATH
common=(-Dbuildtype=debugoptimized -Db_ndebug=true
    -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5 -Dllvm=disabled -Damd-use-llvm=false
    -Dvideo-codecs= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled
    -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dmesa-clc=system -Dallow-fallback-for=libdrm -Dspirv-tools=disabled
    -Dradv-build-id=$build_id)
case $system in
linux-x64)
    [ -f "$libcxx/libc++.so.1" ] || { echo "no libc++ 19 runtime in $libcxx" >&2; exit 2; }
    clang --version | grep -q "clang version 19\." || { echo "clang is not clang 19" >&2; exit 2; }
    CC=clang CXX=clang++ meson setup --wipe "$build" "$mesa" "${common[@]}" \
        "-Dc_args=-march=x86-64-v3 -mtune=znver2" \
        "-Dcpp_args=-march=x86-64-v3 -mtune=znver2 -nostdinc++ -isystem $sdk_cxx" \
        "-Dcpp_link_args=-nostdlib++ -L$libcxx -Wl,-rpath,$libcxx -lc++ -lc++abi"
    target=src/amd/vulkan/libvulkan_radeon.so ;;
windows-x64)
    "$mingw/bin/clang" --version | grep -q "clang version 19.1.7" || { echo "not llvm-mingw's clang 19.1.7: $mingw" >&2; exit 2; }
    mkdir -p "$build.cross"
    cat > "$build.cross/cross.txt" <<EOF
[binaries]
c = '$mingw/bin/x86_64-w64-mingw32-clang'
cpp = '$mingw/bin/x86_64-w64-mingw32-clang++'
ar = '$mingw/bin/llvm-ar'
strip = '$mingw/bin/llvm-strip'
windres = '$mingw/bin/x86_64-w64-mingw32-windres'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    meson setup --wipe "$build" "$mesa" --cross-file "$build.cross/cross.txt" "${common[@]}" -Dzlib:default_library=static \
        "-Dc_args=['-march=x86-64-v3','-mtune=znver2','-mno-ms-bitfields']" \
        "-Dcpp_args=['-march=x86-64-v3','-mtune=znver2','-mno-ms-bitfields','-nostdinc++','-isystem','$sdk_cxx']" \
        "-Dc_link_args=['-l:libwinpthread.a']" "-Dcpp_link_args=['-l:libwinpthread.a']"
    target=src/amd/vulkan/vulkan_radeon.dll ;;
*) echo "make_prepare_driver.sh: --system linux-x64 or windows-x64" >&2; exit 2 ;;
esac
printf '%s\n' "$build_id" > "$build/.radv-build-id"
if [ -n "$lock" ]; then
    flock "$lock" nice -n 19 ninja -C "$build" -j"$jobs" "$target"
else
    nice -n 19 ninja -C "$build" -j"$jobs" "$target"
fi
echo "built $build/$target (build ID $build_id)"
