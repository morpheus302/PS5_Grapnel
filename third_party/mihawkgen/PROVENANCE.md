# mihawkgen

PS5_MihawkGen's frame generator ("PS5_FrameGen"), by Mihawk: a small neural network in Vulkan compute shaders that makes a
picture between two of a program's pictures from their colour alone. GPL-3.0-or-later (`LICENSE.md`, that repository's
licence file, which says what it covers; the files kept here are all under its GPL-3.0-or-later part).

- From `mihawk-99/PS5_MihawkGen`, commit `c57232a9f16bcb6df5fe07c9bb631518f2b1cc0e` ("Initial commit"), as cloned on this PC
  (`ext/PS5_MihawkGen`, no network access when copied), unmodified:
  - `framegen/include/framegen.h`, the generator's interface;
  - `framegen/src/` whole: `fg_common.cpp`, `fg_vulkan.cpp`, `fg_neural.cpp`, `fg_neural.h`, `fg_splat.cpp`, `fg_splat.h` and
    `shaders/*.comp.h` (the SPIR-V the sources compile in);
  - `framegen/shaders/*.comp` and `*.glsl`, the GLSL those SPIR-V headers were compiled from (kept for the licence; not built);
  - `framegen/models/network.fgw` (1,757,240 bytes), the trained network's weights, read at run time.
  - `THIRD_PARTY_NOTICES.txt`, that repository's own notices (grapnel, 23/11/2026): the whole repository's, so it also
    names parts not kept here (`base/`, `external/`); kept for its notes on the public models the network was
    trained against (RIFE and IFRNet MIT, EMA-VFI, FILM and VFIMamba Apache-2.0) and on trademarks.
- Built into the console's Vulkan back end by `nx2/build.py` (PS5 only) and into the PC's Linux renderer by
  `build/merge3/build-vk-linux.sh`: the four `framegen/src/*.cpp` with `gpu/vk/mihawkgen_glue.h` forced in (their Vulkan calls
  through the back end's loaded functions, three of them counted or guarded there), and `gpu/vk/vk_framegen.cpp` with
  `HOOKSHOT_MIHAWKGEN`. Used only with `RX_VK_MIHAWKGEN=1`. `network.fgw` goes in the console's title folder
  (`framegen/network.fgw`, `nx2/toolchain_ps5.py`).
- The wiring follows PS5_Hookshot (`ext/ps5-hookshot-mihawk`, `gpu/vk/vk_framegen.cpp`) and heydemoura's PS5_Longshot
  (`ext/ps5-longshot-heydemoura`, its `HOOKSHOT_MIHAWKGEN` block); it is ours, in `gpu/vk/`, not here.

tree-sha256: 8356ac69e23d05cd7508fdc3842c9a406fe46b626570ea43d95337628bbec2f9
