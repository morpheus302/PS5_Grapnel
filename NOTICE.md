# Notices

## What Grapnel is, in licence terms

Grapnel is a modified version of **Longshot** ([git.magicbean.dev/MagicBeanDev/Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)),
from its commit `ac7787515f7c4ca9dc48d70cad74dc009f0172d1` of 2026-11-16, with some later Longshot changes carried over.
Longshot is itself a modified version of **Hookshot**, by NotRexed. Grapnel ports it to the PS5.

This repository is a snapshot without its development history. What was changed, and when, is listed file by file in
[docs/grapnel-changes.md](docs/grapnel-changes.md) (section 5a of the GPL-3.0); inside the files, comments marked `(ps5)`,
`(hookshot-ps5)` or `(grapnel)` say what the port changed and why.

    Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
    Longshot: Copyright (C) 2026 the Longshot contributors
    Grapnel:  Copyright (C) 2026 morpheus302 and the Grapnel contributors

Grapnel is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version
([LICENSE](LICENSE)). It is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details.

The repository holds no game code, game data, keys or programs built from them, and no executable for the console.
What the builder makes from your own copy of the game (`eboot.bin`, `image.bin`, the controller icons, the caches) is
yours alone: never share it.

## Third-party parts in this repository

Each keeps its own licence and notices, in its folder or its files' headers. All of them can be combined with the
GPL-3.0.

| Part | Where | Licence | Notice |
|---|---|---|---|
| Longshot and Hookshot | the whole tree | GPL-3.0-or-later | `LICENSE`, [README.md](README.md) "License" |
| Eden's shader recompiler, Eglantine's Ampere lowering | `gpu/vk/eden/`, `gpu/third_party/suyu/shader_recompiler/frontend/ampere/` | GPL-3.0-or-later (GPL-2.0-or-later files among them) | files' headers, `gpu/PROVENANCE.md` |
| suyu (shader recompiler, decoders, AArch64 translator) | `gpu/third_party/suyu/`, `third_party/a64-kit/lift/suyu/` | GPL-2.0-or-later / GPL-3.0-or-later per file | texts in those folders |
| a64-kit (NotRexed), with changes from the Triforce project's translator v0.4.0 | `third_party/a64-kit/` | GPL (through suyu); Triforce's changes GPL-3.0-or-later | `third_party/a64-kit/NOTICE.TOTKPort.md`, the "from Triforce v0.4.0" comments |
| sirit | `gpu/vk/sirit/` | its headers say BSD-3-Clause; its folder carries an AGPL-3.0 `LICENSE` and a GPL-3.0 `LICENSE.txt` (as inherited from Longshot) | those files; any of them is compatible here |
| `nvn_api.h` (h1k421, adapted by jhaizhou), `renderdoc_app.h` | `gpu/render/` | MIT | `gpu/PROVENANCE.md` |
| RenoDX, PsychoV35 (Carlos Lopez) | `gpu/vk/eden/shader_recompiler/ir_opt/` | MIT | `LICENSE-renodx.md` |
| PS5_MihawkGen's frame generator (Mihawk), unmodified | `third_party/mihawkgen/` | GPL-3.0-or-later | `LICENSE.md`, `THIRD_PARTY_NOTICES.txt`, `PROVENANCE.md` |
| The storage broker, adapted from Forever Launcher's (bizkut/ps5-forever `0b6abf0`) | `host/posix/ps5_broker_payload.c`, `ps5_broker.h`, `ps5_broker_client.c` | GPL-3.0-or-later | the files' headers |
| Controller and sound, after PS5_VulkanTemplate's `ps5/src/platform.c` | `host/posix/hos_sdl_ps5.c` | MIT (Sascha Willems, Mihawk) | the file's header |
| Player 1's DualSense microphone, after PS5_RetroArch's `src/microphone_ps5.cpp`, `tests/microphone_ps5_test.cpp` and `tooling/ps5-stubs/audio_in_link_stub.c` (Mihawk, mihawk-99/PS5_RetroArch `6f03aa22`) | `host/posix/ps5_mic.h`, the microphone's part of `host/posix/hos_sdl_ps5.c`, `ps5/stubs/libSceAudioIn_link_stub.c`, `tests/test_ps5_microphone.py` | GPL-3.0-or-later | the files' headers |
| The TSC clock, after Mihawk's `6572e0c0` in heydemoura/PS5_Longshot; the in-place readback parts after BXYMartin's PR #49 there | `host/posix/tsc_clock.h`; `gpu/vk/vk_memory.cpp`, `vk_replay.cpp`, `tests/test_readback_part.py` | GPL-3.0 | the comments in those files |
| Dear ImGui 1.92.9b | `third_party/imgui/` | MIT | its `LICENSE.txt` |
| zstd 1.5.7 | `third_party/zstd/` | BSD (dual BSD / GPL-2.0, taken as BSD) | its `PROVENANCE.md` |
| zbic (Atmosphère's zstd with ZBIC) | `third_party/zbic/` | GPL-2.0 (a PC tool of the builder, not linked into the game) | its folder |
| libopus 1.5.2 | `third_party/opus/` | BSD-3-Clause | its `COPYING` |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1 shaders), FSR API 2.4.0 headers | `third_party/fidelityfx-sdk/`, `third_party/fidelityfx/` | MIT | their folders |
| Intel XeSS headers | `third_party/xess/` | MIT | its folder |
| SDL's notices | `third_party/sdl2/` | zlib | its folder (not used on the PS5) |
| libretro's CRT shaders | `third_party/crt_shaders/` | MIT, GPL-2.0-or-later, per file | its `PROVENANCE.md` |
| Libertinus fonts 7.051 | `third_party/fonts/libertinus/` | SIL Open Font License 1.1 | `OFL.txt` |
| Kenney Input Prompts 1.5 | `third_party/kenney_input/`, `third_party/kenney_input_keyboard/` | CC0 | `License.txt` |
| Pipeline lists, the PGO profile | `assets/pipeline_states/`, `assets/profiles/` | contributions to Longshot, no game content | their READMEs |
| Grapnel's pictures (the grapnel logo and icon) | `assets/grapnel.png`, `builder/setup/grapnel.ico` | GPL-3.0-or-later, as the rest of Grapnel | drawn for Grapnel from simple shapes |

Longshot's own logo, its website (`website/`) and its branding pictures (`assets/branding/`) carry no licence and are not
part of this repository; neither is the save Longshot keeps in `assets/saves/`.

## What the console build uses from outside this repository

Not included, pinned in [ps5/DEPENDENCIES.md](ps5/DEPENDENCIES.md): mihawk-99's PS5_PayloadSDK (on ps5-payload-dev's SDK
v0.42), PS5_Vulkan (with its `runtime/libc.prx`, clean-room), PS5_VulkanTemplate and PS5_Mesa (GPL-3.0-or-later, MIT);
dav1d 1.5.1 (BSD-2-Clause, linked into the eboot: Copyright © 2018-2019, VideoLAN and dav1d authors, as its `COPYING` says); Vulkan-Headers,
SPIRV-Headers and the Vulkan Memory Allocator (Apache-2.0 OR MIT, MIT); LLVM 19.1.7's clang, lld, libc++, libc++abi and
libunwind (Apache-2.0 WITH LLVM-exception; the last three linked statically, from the SDK). The Vulkan driver in the eboot
is built from PS5_Mesa and Grapnel's seven patches in [ps5/mesa-patches/](ps5/mesa-patches/README.md) (MIT).

## References to unpublished notes

Comments and documents here sometimes point to `notes-agents/…`, `logs/udp/…`, `PLAN.md`, `REPRISE.md`, `hookshot-ps5/…`,
`dist/next/…`, `build/lot…` or a "lot NN". They name the author's working notes, console logs and work batches, which are
not published; the comment itself says what matters. "Hookshot PS5" and `hookshot-ps5` are the port's earlier name.

## Trademarks

The Legend of Zelda and Ocarina of Time are trademarks of Nintendo. PlayStation, PS5 and DualSense are trademarks of Sony
Interactive Entertainment Inc. AMD, Radeon, FSR and FidelityFX are trademarks of Advanced Micro Devices, Inc.; NVIDIA and
DLSS of NVIDIA Corporation; Intel and XeSS of Intel Corporation; Vulkan of the Khronos Group Inc. Grapnel is not
affiliated with, endorsed or sponsored by any of them.
