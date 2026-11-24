# The shaders' preparation's driver for Windows: four more changes to RADV (PS5_Mesa)

The builder prepares the game's shaders on the player's PC (step 5) with the console's driver built for that PC on its
PS5 winsys's host model (`radv_ps5_platform.c`'s PC branch): the cache it fills has the console driver's keys and bytes
(checked on a player's Windows PC on 2026-11-24: the same 57,210 keys and contents as Linux's cache). On Linux that driver
is PS5_Mesa with the seven patches of [`../`](../README.md); on Windows it needs these four more, on top of the seven:

| # | Patch | What it does |
|---|---|---|
| 1 | `0001-meson-allow-the-shader-cache-on-Windows-Grapnel-s-pr.patch` | Mesa's meson refuses the shader cache on Windows: allowed |
| 2 | `0002-MinGW-uint-for-the-shared-CL-C-headers-drm.h-without.patch` | MinGW: `uint` for the headers shared with OpenCL C, `drm.h` without `sys/ioccom.h`, the Vulkan runtime's Win32 entry points declared |
| 3 | `0003-Windows-MinGW-the-host-model-s-memory-over-VirtualAl.patch` | The host model's memory over `VirtualAlloc` (Linux keeps its `mmap`: the same code, regrouped), and the shader cache's POSIX calls over a small shim, `src/util/grapnel_win_posix.h` (`flock` as `LockFileEx` on a byte far past the file's end, `mmap` of the index as a file mapping), turned on by `GRAPNEL_WIN_CACHE` |
| 4 | `0004-Windows-MinGW-the-cache-s-last-POSIX-calls-fstatat-s.patch` | The cache's last POSIX calls (`fstatat`, `st_blocks`, `fcntl` locks, `passwd`), and no Win32 or DXGI presentation in this driver, which presents nothing |

None changes a line Linux or the console compiles (each is under `_WIN32`, `GRAPNEL_WIN_CACHE` or a meson test of the
host system; patch 3 regroups the host model's `mmap` calls into functions with the same calls). **License:** Mesa's
(MIT), as the files they change; Grapnel: Copyright (C) 2026 morpheus302 and the Grapnel contributors.

## Build (Linux and Windows)

[`scripts/grapnel-builder/make_prepare_driver.sh`](../../../scripts/grapnel-builder/make_prepare_driver.sh) is the recipe of
both, from PS5_Mesa `0f86253f674c425052948eb3cb116c81e646a734` with `../0001` to `../0007` applied (and these four on top
for Windows):

```bash
git clone https://github.com/mihawk-99/PS5_Mesa.git mesa && cd mesa
git checkout 0f86253f674c425052948eb3cb116c81e646a734
git apply --index <grapnel>/ps5/mesa-patches/0*.patch
git apply --index <grapnel>/ps5/mesa-patches/windows/0*.patch        # (Windows only)
<grapnel>/scripts/grapnel-builder/make_prepare_driver.sh --system linux-x64 --mesa . --build ../host-linux \
    --sdk-cxx <payload SDK>/target/include/c++/v1 --libcxx <libc++1-19 unpacked>/usr/lib/x86_64-linux-gnu --clc-bin <dir>
<grapnel>/scripts/grapnel-builder/make_prepare_driver.sh --system windows-x64 --mesa . --build ../host-windows \
    --sdk-cxx <payload SDK>/target/include/c++/v1 --mingw <llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64> --clc-bin <dir>
```

What makes the cache the console's, besides the build ID (`1599cbb5b1a0de5dc9beb4d7026883a49e9b5b63`, the console archive's):
clang 19, `-march=x86-64-v3 -mtune=znver2`, ACO compiled under the payload SDK's **libc++ 18.1** headers (the console's), and on
Windows **`-mno-ms-bitfields`**: MinGW lays bitfields out as MSVC does by default, and RADV hashes and stores structs with
bitfields; with MSVC's layout a test pipeline's cache entries differed, and a whole preparation shared 18 % of its keys with
Linux's. `mesa_clc` and `vtn_bindgen2` are built for the PC from the same PS5_Mesa revision first (PS5_Vulkan's
`tools/build-radv.sh` makes them).

Then `scripts/grapnel-builder/make_prepare_tool.py --system linux-x64|windows-x64 --mesa-build <build>` makes the
toolchain's `prepare/` (the console's renderer and a host with no game code, this driver, their licences), and
`make_toolchain.py --prepare <it>` puts it in the toolchain.
