# Credits

Grapnel is a small layer on a great deal of other people's work. Thank you all.

## The game's recompilation

- **[Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)** and its contributors (Magic Bean): everything Grapnel
  runs on the console but the PS5 layer: the translation of the game's code to C, the reimplemented system software, the
  Vulkan renderer, the menus. Longshot's own credits, in its [README](README.md#credits), carry over in full.
- **Hookshot** by **NotRexed**, which Longshot is a fork of, and the a64-kit tools. (Hookshot's GitHub repository is no
  longer online; Longshot carries its history.)
- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **Eglantine** (the shader recompiler and its Switch 2 work), and
  **suyu** (the graphics decoders and the AArch64 translator), as Longshot credits them.
- **The [Triforce](https://git.triforceproject.com/triforce/source) project**: Grapnel's translator carries two changes
  from its v0.4.0 (`A64_FPX_NC`, and FMUL/FMLA by element), marked in `third_party/a64-kit/lift/a64_lift.py`.

## The PS5

- **[mihawk-99](https://github.com/mihawk-99)** (Mihawk), without whom there would be no Vulkan on a jailbroken PS5 for
  this to run on: [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (RADV on the PS5), [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
  (the link, the native tool, `libc.prx`), [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) (the platform
  layer), [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) and [PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen)
  (the frame generator, in `third_party/mihawkgen/`). His PS5_Hookshot was the model for the frame generator's wiring,
  his TSC clock (`host/posix/tsc_clock.h`) came by way of heydemoura's PS5_Longshot, and the ocarina hears player 1's
  DualSense microphone after his [PS5_RetroArch](https://github.com/mihawk-99/PS5_RetroArch)'s (`host/posix/ps5_mic.h`).
- **[ps5-payload-dev](https://github.com/ps5-payload-dev)**: the payload SDK (v0.42) PS5_PayloadSDK is built on, and the
  ELF loader the title sends its storage broker to.
- **BlackBearReloaded**: ps5-native-app-boilerplate, after which PS5_Vulkan generates `libc.prx`.
- **[heydemoura/PS5_Longshot](https://github.com/heydemoura/PS5_Longshot)** (Heyde Moura): the PS5 port of Longshot whose
  builder is the model for Grapnel's (its kit, its steps, its checks), and **BXYMartin**, whose PR #49 there brought the
  in-place readback parts Grapnel uses (`RX_VK_READBACK_IN_PLACE_PART`).
- **[bizkut](https://github.com/bizkut)**: Forever Launcher's write broker (ps5-forever), which Grapnel's storage broker is
  adapted from, and Savage Wind, which showed that a recompiled game could run on a jailbroken PS5.
- **[mateo-cogeanu/ProsperoCraft](https://github.com/mateo-cogeanu/ProsperoCraft)**: its notes on the console's microphone
  service, which showed that the port is best made early and kept.
- **[drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, **kstuff** and **elfldr**: the console-side
  tools Grapnel needs, each by its own authors.

## Parts and assets

- **[Kenney](https://kenney.nl)**: the Input Prompts icons (CC0) the console's controller icons are drawn with.
- **Libertinus** (SIL OFL 1.1): the menus' fonts, and the one Grapnel's pictures are lettered in.
- **[Dear ImGui](https://github.com/ocornut/imgui)** (Omar Cornut), **[dav1d](https://code.videolan.org/videolan/dav1d)**
  (VideoLAN), **[zstd](https://github.com/facebook/zstd)**, **[Opus](https://opus-codec.org)**, the Vulkan headers and the
  Vulkan Memory Allocator, and the other parts listed with their licences in [NOTICE.md](NOTICE.md).
- **[bc7enc](https://github.com/richgel999/bc7enc)** (Rich Geldreich, MIT): the encoder the home screen's background
  pictures were packed with.

## The port itself

Grapnel is by **morpheus302**, written with the help of Claude (Anthropic): most of the port's code, tests and notes were
drafted by Claude and reviewed, built and tried on a base PS5 by morpheus302.
