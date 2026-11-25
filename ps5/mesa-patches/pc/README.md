# The shaders' preparation's driver on a PC: one more change to RADV (PS5_Mesa)

The builder prepares the game's shaders on the player's PC with the console's driver built for that PC, on its PS5
winsys's host model (`radv_ps5_platform.c`'s PC branch). This change is for that PC driver only, Linux and Windows, on top
of [`../0001` to `../0007`](../README.md) (and of [`../windows/`](../windows/README.md) for Windows):

| # | Patch | What it does |
|---|---|---|
| 1 | `0001-radv-ps5-the-host-model-flushes-with-CLFLUSH-which-every-x86.patch` | Off the console, `radv_ps5_cpu_flush` writes its lines back with `CLFLUSH`, which every x86-64 CPU has, instead of `CLFLUSHOPT`, which Intel's CPUs before Skylake and AMD's before Zen do not have: the preparation stopped there at once (exit 0xC000001D on an i5-4670K, 2026-11-25) |

It changes no line the console compiles (`#if defined(__PROSPERO__)` keeps `CLFLUSHOPT` there), nothing a shader cache key
or entry holds, and nothing a GPU runs (the host model runs none). Grapnel 0.2.4's toolchain (0.3.3) is the first with it.
**License:** Mesa's (MIT), as the file it changes; Grapnel: Copyright (C) 2026 morpheus302 and the Grapnel contributors.

## Apply

```bash
git clone https://github.com/mihawk-99/PS5_Mesa.git mesa && cd mesa
git checkout 0f86253f674c425052948eb3cb116c81e646a734
git apply --index <grapnel>/ps5/mesa-patches/0*.patch
git apply --index <grapnel>/ps5/mesa-patches/windows/0*.patch        # (Windows only)
git apply --index <grapnel>/ps5/mesa-patches/pc/0*.patch
```

then [`scripts/grapnel-builder/make_prepare_driver.sh`](../../../scripts/grapnel-builder/make_prepare_driver.sh), as for
the others.
