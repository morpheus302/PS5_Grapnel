#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# LLVM 19.1.7 for Windows as the Grapnel builder ships it (make_windows_package.py --llvm-build): clang, lld, llvm-ar, llvm-nm,
# llvm-objcopy, built on this Linux PC by clang 19.1.7 (llvm-mingw 20250114's) against WinLibs' GCC 14.2 runtime (its
# libstdc++, MSVCRT, winpthreads), everything static: one clang.exe that loads no DLL but Windows' own.
#
# Why: WinLibs' own LLVM (the 0.2 and 0.3 packages') is built with BUILD_SHARED_LIBS, about 150 DLLs, by GCC; on the player's
# Ryzen 5 3600X it compiled the lifted code 25-30 % slower than llvm-mingw's. llvm-mingw's LLVM is built on libc++ and compiles
# gpu/vk/vk_texture.cpp otherwise than every libstdc++-built clang 19.1.7 (Debian's, LLVM's Linux release, WinLibs'): the eboot
# differed. Built on libstdc++ the code is Linux's; built by clang and static it is as fast as it gets.
#   -femulated-tls  GCC for MinGW keeps libstdc++'s thread_local variables (std::__once_callable...) in emulated TLS; clang for
#                   MinGW would ask for native TLS symbols libstdc++.a does not have
#   X86 only, no zlib/zstd/libxml2/terminfo: nothing the builder uses; Debian's llvm-tblgen-19 and clang-tblgen-19 run here
#
#   make_windows_llvm.sh --source <llvm-project-19.1.7.src> --sysroot <WinLibs' mingw64, its include/ and lib/> \
#       --mingw <llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64> --build <folder> [--jobs N] [--lock F]
#   -> <build>/bin/{clang,ld.lld,llvm-ar,llvm-nm,llvm-objcopy}.exe, <build>/lib/clang/19/include
#
# The source is llvm-project-19.1.7.src.tar.xz (sha256 82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501), the
# sysroot WinLibs' winlibs-x86_64-posix-seh-gcc-14.2.0-llvm-19.1.7-mingw-w64msvcrt-12.0.0-r3.7z (make_windows_package.py's
# pinned download) unpacked: mingw64/include, mingw64/lib, mingw64/x86_64-w64-mingw32.
set -euo pipefail
jobs=$(nproc) lock=
while [ $# -gt 0 ]; do
    case $1 in
        --source) src=$2; shift 2 ;;
        --sysroot) sysroot=$2; shift 2 ;;
        --mingw) mingw=$2; shift 2 ;;
        --build) build=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --lock) lock=$2; shift 2 ;;
        *) echo "make_windows_llvm.sh: unknown $1" >&2; exit 2 ;;
    esac
done
grep -q 'set(LLVM_VERSION_MAJOR 19)' "$src/cmake/Modules/LLVMVersion.cmake" &&
    grep -q 'set(LLVM_VERSION_PATCH 7)' "$src/cmake/Modules/LLVMVersion.cmake" || { echo "not LLVM 19.1.7: $src" >&2; exit 2; }
[ -f "$sysroot/lib/gcc/x86_64-w64-mingw32/14.2.0/libgcc.a" ] || { echo "not WinLibs' GCC 14.2 sysroot: $sysroot" >&2; exit 2; }
c=$mingw/bin
"$c/clang" --version | grep -q "clang version 19.1.7" || { echo "not clang 19.1.7: $c/clang" >&2; exit 2; }
x="--target=x86_64-w64-mingw32 --sysroot=$sysroot -femulated-tls"
link="-fuse-ld=lld -static -rtlib=libgcc -stdlib=libstdc++ -lpthread"
cmake -G Ninja -S "$src/llvm" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
    -DCMAKE_C_COMPILER="$c/clang" -DCMAKE_CXX_COMPILER="$c/clang++" -DCMAKE_RC_COMPILER="$c/llvm-windres" \
    -DCMAKE_AR="$c/llvm-ar" -DCMAKE_RANLIB="$c/llvm-ranlib" \
    -DCMAKE_C_FLAGS="$x" -DCMAKE_CXX_FLAGS="$x -stdlib=libstdc++" -DCMAKE_ASM_FLAGS="$x" \
    -DCMAKE_EXE_LINKER_FLAGS="$link" -DCMAKE_SHARED_LINKER_FLAGS="$link" \
    -DCMAKE_FIND_ROOT_PATH="$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-w64-windows-gnu \
    -DLLVM_NATIVE_TOOL_DIR=/usr/lib/llvm-19/bin \
    -DLLVM_TABLEGEN=/usr/lib/llvm-19/bin/llvm-tblgen -DCLANG_TABLEGEN=/usr/lib/llvm-19/bin/clang-tblgen \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_Z3_SOLVER=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCLANG_VENDOR="Grapnel"
targets="clang lld llvm-ar llvm-nm llvm-objcopy"
if [ -n "$lock" ]; then
    flock "$lock" nice -n 19 ninja -C "$build" -j"$jobs" $targets
else
    nice -n 19 ninja -C "$build" -j"$jobs" $targets
fi
