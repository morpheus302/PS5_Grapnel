# Grapnel builder 0.2.3 (Linux): building Grapnel for your PS5

*Version française : [INSTALL.fr.md](INSTALL.fr.md).*

*On Windows, take `Grapnel-Builder-0.2.3-windows-x64.zip` from the Releases page instead: a folder with everything in it and a
step-by-step window (its README, and "Build it on Windows" in the repository's README). This guide is the Linux one.*

Grapnel is a port to the PS5 of Longshot, the static recompilation of *The Legend of Zelda: Ocarina of Time* (Nintendo Switch 2
Edition). The PS5 title holds the game's code translated and compiled, so it cannot be downloaded: you build it on your own
Linux PC from **your own copy of the game**, with this builder. Nothing it makes may be shared.

## What you need

- **Your game file**: the `.nup` dump of *Zelda: Ocarina of Time*, Nintendo Switch 2 Edition, **with its update v131072**
  (title `0400486024DEC800`; some dumps keep the update under the base game's title, `0400486024DEC000`, in the file and its
  name: that is fine too, the builder checks the game's program itself). The
  builder takes that `.nup` only, and refuses any other game or version.
- **A Linux PC** (64-bit x86, Ubuntu 24.04, Debian 13 or newer), **8 GB of memory** at the least (16 GB or more is faster),
  **10 GB of free disk** besides the `.nup` (and about 2 GB more for LLVM), and 45 minutes to 2 hours and a half.
- **LLVM 19.1.7** exactly (every player compiles with the same compiler, so every eboot is the one the project tested),
  **Python 3.12 or newer** (the builder's packages need it) and **git**: how to get them is below.
- **The console toolchain**, `grapnel-toolchain-0.3.2-linux-x64.tar.xz`, from the **Releases** page of the Grapnel repository
  (<https://github.com/morpheus302/PS5_Grapnel/releases>): the PS5 payload SDK, PS5_Vulkan's linker parts, the RADV driver,
  the renderer's headers, dav1d, and `prepare/`, the shaders' preparation for the PC. No game file and no game code in it.
- On the console: a jailbreak with an ELF loader and access to `/data`, and FTP (or another way to copy files).
  Grapnel is played on a base PS5 (firmware up to 12.60). **The PS5 Pro was never tried.**

## Install the tools (once)

1. **LLVM 19.1.7, git and Python's `venv`.**
   - Ubuntu 24.04: LLVM's own packages, from <https://apt.llvm.org> (Ubuntu's own `clang-19` is 19.1.1, which the builder
     refuses; apt.llvm.org's LLVM 19 is 19.1.7, with everything the builder needs):

     ```sh
     sudo apt update
     sudo apt install git python3-venv wget lsb-release software-properties-common gnupg
     wget https://apt.llvm.org/llvm.sh
     chmod +x llvm.sh
     sudo ./llvm.sh 19
     ```
   - Debian 13 (its LLVM 19 is 19.1.7):

     ```sh
     sudo apt install git python3-venv clang-19 lld-19 llvm-19 libclang-rt-19-dev
     ```
   - Others: the LLVM 19.1.7 release archive for Linux x86-64 from
     <https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.7>, unpacked anywhere (then give the builder
     `--llvm <that folder>`), with git and Python 3.12 or newer (and its `venv` module) from your system.
2. **The Grapnel repository**, with `git clone` (so the build carries its exact version; a ZIP download says `unknown`):

   ```sh
   git clone https://github.com/morpheus302/PS5_Grapnel
   cd PS5_Grapnel
   ```
   Every command below runs in this folder.
3. **The builder's Python packages**, in a Python environment of their own (Ubuntu and Debian refuse `pip install` outside
   one):

   ```sh
   python3 -m venv ~/grapnel-venv
   ~/grapnel-venv/bin/pip install -r scripts/grapnel-builder/requirements.txt
   ```
4. **The console toolchain**: download `grapnel-toolchain-0.3.2-linux-x64.tar.xz` from the Releases page, then unpack it, here
   into your home folder (it makes `~/grapnel-toolchain-0.3.2-linux-x64`):

   ```sh
   tar -xf ~/Downloads/grapnel-toolchain-0.3.2-linux-x64.tar.xz -C ~
   ```
   Keep the folder as it is unpacked, and writable: every build checks its files, and writes one of its own in it.

## Check, then build

From the `PS5_Grapnel` folder:

```sh
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --check --toolchain ~/grapnel-toolchain-0.3.2-linux-x64
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --nup ~/games/zelda-oot.nup --out ~/grapnel-out \
    --toolchain ~/grapnel-toolchain-0.3.2-linux-x64
```

A game file whose name has spaces or brackets goes in quotes, with `$HOME` rather than `~` (the shell does not expand a `~`
in quotes): `--nup "$HOME/games/my game [v131072].nup"`.

`--check` needs no game and says what is missing: all is well when it ends with `@@done check` (a line starting `note:` says
something worth knowing, not an error).
The build prints its eight steps and ends with `@@done <out>`; the longest are the compile (about 25
minutes on a recent 8-core PC) and **the shaders' preparation** (about 15 minutes on a recent 8-core PC, up to 45 on a small
one; 2 GB of free memory at the least): the PC compiles the game's shaders for the console's driver, on every core, as many at
once as its free memory allows, and shows the time left. With little memory, add `--lift-jobs 1 --memory 4`: slower, never
different. Building again after an update of the repository is much shorter (the build's own files stay in `<out>/grapnel-work`;
the preparation is made again only when the renderer or the lists changed).

## Copy to the console

`<out>/INSTALL.en.txt` says it again for what was built:

1. `<out>/homebrew/GRPN00001` → `/data/homebrew/GRPN00001` (with its `radv-shader-cache/`, the prepared shaders: ~210 MB),
   then register the title with your homebrew tools: it shows as **Grapnel** on the home screen. **After a new build**, delete
   the console's `/data/homebrew/GRPN00001/radv-shader-cache` before you copy the folder.
2. `<out>/data/grapnel` → `/data/grapnel` (`image.bin`, `hookshot_env.txt`, `skip.txt`, `prompts/`,
   `hookshot_prepare_progress.txt`, and `hookshot_menu_fond.rgba`, the launch screen's picture).
   **If the console already has `/data/hookshot`** (a Hookshot port): do not make
   `/data/grapnel`; copy `image.bin`, `prompts/` and `hookshot_prepare_progress.txt` into `/data/hookshot`. Grapnel's first
   start renames it to `/data/grapnel`, saves and all; a `/data/grapnel` made beforehand would be used instead, and the saves
   left behind.
3. Your `.nup`, as it is → `/data/grapnel/game.nup`.
4. Start Grapnel. **The shaders are ready**: the builder prepared them on your PC, into the console driver's own cache (the
   title folder's `radv-shader-cache`), so the game does not stop to compile them as you play (each compile in play is a stop
   of a second or more, and memory the console never gets back). The console prepares nothing: the launch screen only checks
   the cache at start (its SHADERS page says "Ready"). A pipeline no list knows is still compiled the first time it is drawn.

`eboot.bin` and `image.bin` go together: after every new build, copy both again.

## Private

`<out>/` and its `grapnel-work/` hold the game's code and data made from your copy (`radv-shader-cache/` is the game's shaders
compiled, `grapnel-work/shaders/` their code): **never share, upload or post them**, not
even a part. Bug reports: the log (`/data/grapnel/hookshot_log.txt`) and `<out>/manifest.json` (sizes and checksums only) are
fine; nothing else from `<out>`.

## What the builder does

It reads the game's program out of the `.nup` (40 MB of its 25.6 GB), translates it to C with the port's lifter, compiles it
with the console's SDK, links it with Grapnel's runtime and renderer, signs the eboot and lays out the title folder (the title's
memory, 580 MiB of flexible memory, is fixed), then makes the PlayStation button icons from the game's own font and textures.
Last, it prepares the shaders: it lists the game's shaders (from the ~290 MB of the `.nup` that hold them) and the pipelines to
build (from the public lists in this repository: no game file), then runs **the console's own shader preparation on your PC**,
on a model of the console's driver built for Linux (the same driver, so the same cache keys), and checks the cache it made: every
part whole, and a second pass that rebuilds everything and finds it all there, compiling nothing. The toolchain's `prepare/`
holds no game code. Linux x86-64 only for now; `--no-prepare` leaves it out (the console then compiles in play).
Its source is `scripts/grapnel-builder/grapnel_build.py`; how it was checked: two builds from different folders give the same
eboot, byte for byte.
