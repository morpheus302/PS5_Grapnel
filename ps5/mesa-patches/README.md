# Grapnel's changes to RADV (PS5_Mesa)

The Vulkan driver linked into Grapnel's `eboot.bin` is RADV for the PS5 from
[mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), with the seven changes in this folder on top. Without them the
driver is not the one Grapnel was built and tested with on the console: publish both together.

- **Base:** PS5_Mesa `0f86253f674c425052948eb3cb116c81e646a734` (2026-11-10, "radv/ps5: the mesh shaders' scratch ring is
  128 MiB, not 320"), a commit of PS5_Mesa's `main`.
- **On top, in order:** the seven patches below, 18 files, +1,006 / -151 lines. Applied to the base, they give exactly the
  tree the console's driver was built from (checked by applying them to the base in a clean index and comparing the tree
  with our branch's: the same tree object).
- **License:** the files they change are Mesa's, under Mesa's licence (MIT); the changes are offered under the same terms.
  Grapnel: Copyright (C) 2026 morpheus302 and the Grapnel contributors.

| # | Patch | Files | What it does |
|---|---|---|---|
| 1 | `0001-radv-ps5-an-emulated-indirect-task-draw-costs-what-its-count.patch` | 11 | An emulated indirect task draw costs what its count buffer draws, in chunks of `RADV_PS5_TASK_WORKGROUPS` workgroups |
| 2 | `0002-ac-nir-ngg_mesh-a-mesh-shader-that-never-sets-its-outputs-em.patch` | 1 | A mesh shader that returns without `SetMeshOutputsEXT` emits nothing (its counts start at 0) instead of hanging the GPU |
| 3 | `0003-radv-ps5-the-command-processor-skips-an-emulated-indirect-ta.patch` | 3 | The command processor skips an emulated indirect task draw's empty chunks with flat `COND_EXEC` blocks (`RADV_PS5_TASK_COND_EXEC=0` turns it off) |
| 4 | `0004-radv-ps5-RADV_PS5_LARGE_ALIGN-0-leaves-an-allocation-of-2-Mi.patch` | 3 | An experiment, off unless set: `RADV_PS5_LARGE_ALIGN=0` leaves allocations of 2 MiB or more unaligned to 2 MiB |
| 5 | `0005-radv-ps5-three-experiments-on-waiting-for-the-shaders-RADV_P.patch` | 2 | Switches on waiting for the shaders: `RADV_PS5_SUBMIT_IDLE`, `RADV_PS5_SYNC_CS`, `RADV_PS5_SYNC_PS` (Grapnel's settings use `RADV_PS5_SYNC_CS=1`) |
| 6 | `0006-radv-ps5-narrower-waits-between-draws-late-allocation-and-sh.patch` | 4 | Narrower waits between draws (`RADV_PS5_SYNC_PS=program` or `vertex`), late allocation (`RADV_PS5_LATE_ALLOC`) and NGG culling (`RADV_PS5_NGGC`) switches, and counters for the title's log |
| 7 | `0007-radv-ps5-RADV_PS5_LATE_ALLOC-masks-and-gfx10-make-the-CU-mas.patch` | 4 | `RADV_PS5_LATE_ALLOC=masks` and `gfx10` make the CU masks take effect (Grapnel's settings use `RADV_PS5_LATE_ALLOC=off`) |

Each switch is unset by default and then changes nothing; none changes a compiled shader or the shader cache's key. Each
patch starts with its commit message (what and why); the author lines are left out on purpose, and the patches carry no
other change.

## The shaders' preparation on the PC

The toolchain's `prepare/` runs this driver built for the player's PC on the PS5 winsys's host model, so that the shader
cache it fills is the console's: for Linux from this folder's seven patches, for Windows with the four of
[`windows/`](windows/README.md) on top; both by `scripts/grapnel-builder/make_prepare_driver.sh` (the recipe and why).

## Apply

```bash
git clone https://github.com/mihawk-99/PS5_Mesa.git && cd PS5_Mesa
git checkout 0f86253f674c425052948eb3cb116c81e646a734
git apply --index <grapnel>/ps5/mesa-patches/*.patch      # (or: patch -p1 < each file, in order)
git diff --cached --stat                                   # 18 files changed, 1006 insertions(+), 151 deletions(-)
```

## Build

With the same options and toolchain as PS5_Vulkan's `tools/build-radv.sh release` (PS5_Vulkan `ace98a9`, its cross file
`tooling/radv/ps5-cross.ini` and the payload SDK it installs: `ps5/DEPENDENCIES.md`), from the patched tree, with one more
option, the build ID:

```text
-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5 -Dllvm=disabled -Damd-use-llvm=false
-Dvideo-codecs= -Dbuildtype=debugoptimized -Db_ndebug=true -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false
-Dgles1=disabled -Dgles2=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled
--force-fallback-for=zlib -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false
-Dvulkan-layers= -Dtools= -Dmesa-clc=system
-Dradv-build-id=1599cbb5b1a0de5dc9beb4d7026883a49e9b5b63
```

(`-Dmesa-clc=system`: the host tools `mesa_clc` and `vtn_bindgen2` built from the same PS5_Mesa revision first, as
PS5_Vulkan's script does), then `ninja src/amd/vulkan/libvulkan_radeon.a`, and give the archive to Grapnel's build as
`RADV_ARCHIVE=<build>/src/amd/vulkan/libvulkan_radeon.a`.

- **The build ID** is the key of the driver's shader cache on the console (`radv_device_get_cache_uuid`). Patches 4 to 7
  change nothing a shader is compiled from, so they keep the ID of the build with patches 1 to 3; any 40-digit hex ID
  works, but another one makes the console's driver cache start empty.
- **Release:** assertions off (`-Db_ndebug=true`), as PS5_Vulkan's release build.
- The archive we link (built on our PC from this exact tree) has SHA-256
  `20d88661838a26f89f4098b59e52e1a5d98c1251e3c01d7b414d6a3f557bac1c`. Whether another PC gets the same bytes has not been
  checked.
