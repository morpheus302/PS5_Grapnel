<h1 align="center">
  <img src="../assets/grapnel.png" alt="" height="120"><br>
  Grapnel
</h1>

<p align="center">Longshot's recompilation of a classic console game, running natively on a jailbroken PS5.<br>
Built on your own PC, from your own copy of the game.</p>

<p align="center"><a href="#english"><b>English</b></a> | <a href="#français"><b>Français</b></a></p>

---

## English

### What Grapnel is

Grapnel is a port to the PS5 of [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot), the static recompilation of
the Switch 2 release of *The Legend of Zelda: Ocarina of Time*. The game's code is translated to C and compiled for the
console's own processor; the graphics are rebuilt on Vulkan, through RADV for the PS5
([mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)); the system software the game expects is reimplemented. No
emulator is involved.

All of it happens **on your PC, from your own copy of the game**. This repository holds no game code, game data, keys or
console executable, and does not explain how to get them. What the builder makes from your copy (`eboot.bin`,
`image.bin`, the controller icons, the caches) contains the game: it is for your console only, never share it.

Grapnel is published before it is finished, so that others can help.

### Status and known limits

- It runs on one base PS5, firmware **12.60**, the only firmware it has been tried on. **The PS5 Pro has never been tried.**
- **Not yet a steady 30 fps in the heaviest place.** In the Castle Town market, with the frame rate unlocked, the game
  runs at about 32 frames per second, but about 15 to 25% of its frames take longer than 34 ms: you see stutter there.
  Elsewhere it is smoother. Getting that place to a steady 30 is the main work left.
- **The first start is the hardest.** Until the driver's shader cache has filled, the console compiles the game's GPU
  programs while you play: stutters, and memory that does not come back. In a crowded place this can make the title close
  itself (below). That is why the builder **prepares the shaders on your PC** (about 15 minutes): the console finds them in
  its driver's cache from the first start and prepares nothing; only a pipeline no list knows is still compiled in play. (On
  a PC without AVX2 the console prepares them itself before the first game instead, about an hour.)
- **Long sessions can end at a change of place.** As you play, the memory the game holds grows, and the burst of textures
  loaded when entering a crowded place (Castle Town from Hyrule Field, Kakariko) can leave too little: the title then closes
  itself. Start it again: your save is as you left it. Since 0.2.1 the shaders the console reads from its cache are kept
  compressed (lazy shader modules and freed packed SPIR-V, from rimidalv333's
  [Longshot Oberon](https://github.com/rimidalv333/longshot-oberon)): about 290 MB more free memory (their SPIR-V in 105 MB
  instead of 396), and a player's session ran **36 minutes without closing**, against about 25 before. Making that memory
  last is the other big piece of work left.
- The title watches its own memory: when the console's free memory gets too low, it closes itself and says so in its log,
  rather than let the console run out (early builds could power the whole console off that way).
- Longshot's PC-only tools (bug reports, pipeline lists) are not on the console.

### What you need

- A **jailbroken PS5** (tried on firmware 12.60 only) with **kstuff**, an **ELF loader listening on port 9021** (elfldr),
  and **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) 1.7beta3 or newer** (1.7beta11 tried), which lets the
  title see `/data` and registers it; **1.7beta4 or newer** to install it on an extension disk (it then lets the title see
  that disk too).
- Your local network between the PC and the console: the builder's window can send the title itself (step 6, over FTP), or
  copy it yourself (an FTP server, for example).
- **Your own copy of the game**, dumped from your own console, as a `.nup` file (game version 131072). About 26 GB.
- **A PC with Windows 10 or 11 (64-bit), or Linux on x86-64**, for the builder: 8 GB of memory at the least (16 GB is much
  faster), about 12 GB of free disk space besides the game file. AVX2 is not needed: on a processor without it (x86-64-v3:
  many Intel Celeron, Pentium and Atom processors, older AMDs), the check says so and the builder leaves the shaders to the console,
  which prepares them itself at its first start (about an hour on a standard PS5; on such a PC the preparation would take
  about as long). The build sets it up by itself.
  - **Windows:** `Grapnel-Builder-0.2.6-windows-x64.zip` from this repository's
    [Releases](https://github.com/morpheus302/PS5_Grapnel/releases) page: a folder with everything in it (its LLVM, Python,
    the console toolchain), nothing to install.
  - **Linux:** **LLVM 19.1.7**, git and Python 3.12 or newer, and **the console toolchain**,
    `grapnel-toolchain-0.3.3-linux-x64.tar.xz`, from the same page (the console's SDK, linker parts, driver, the shaders'
    preparation and the FTP server step 6 can send; no game file in it).

### Build it on Windows

1. Unpack the whole `Grapnel-Builder-0.2.6-windows-x64.zip` (right click, "Extract All...") into a folder of yours with a
   short path, `C:\Grapnel-Builder` for instance. Nothing to install, no administrator rights.
2. Start `Grapnel.exe`. It is not signed: if Windows says "Windows protected your PC", click **More info**, then **Run
   anyway**. An antivirus may scan or block the builder's programs (`clang.exe`, `ld.lld.exe`, `zbic_unpack.exe`,
   `ps5-native-tool.exe`), which compile the game for the console: if it removes one, step 3 says the tools are incomplete;
   restore it or exclude the folder, and unpack again.
3. Follow its six steps: **1** your game's `.nup` (checked at once), **2** the output folder (an earlier build there or in
   another folder is offered back), **3** the check of your PC (by itself), **4** the game's build, **5** the shaders,
   prepared on your PC as on Linux, **6** sending it to the console (optional).
4. Step 6 takes the console's IP address. It sends over FTP: the console's FTP server if one answers (port 2121), else
   [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) (GPL-3.0), which it sends to the console's ELF loader (port 9021).
   It first copies your saves to the PC (`console-saves\` in the output folder), asks before it replaces anything, touches
   only the title's folder, the game file's folder and `/data/grapnel`, sends the game file only when the console does not
   have it, carries on where a cut send stopped, and reads the sizes and the eboot's CRC32 back. ShadowMountPlus then
   registers the title.
5. Step 6 installs on the console's **internal storage** or on its **extension disk** (`/mnt/ext1`, found by itself). On
   the disk the title goes to `/mnt/ext1/homebrew/GRPN00001` and the game file to `/mnt/ext1/grapnel/game.nup`; your saves,
   settings and logs stay in `/data/grapnel`, and the `hookshot_env.txt` sent there says where the game file is
   (`HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup`). It never touches the disk's other games. A title or a game file left
   at the other place is removed only if you say yes, once the send is read back.

On a Ryzen 5 3600X with 16 GB (a player's PC, Windows 11), two builds: the translation 155 and 266 s, the compile 1,980 and
2,066 s (33 and 34 min), the shaders' preparation 1,318 and 2,839 s (22 and 47 min, its check included). The `eboot.bin` and the shader cache it made are the Linux builder's, byte
for byte: the same CRC32 for the eboot, the same 57,210 keys and contents for the cache. `Grapnel-console.cmd` runs the same
build without the window. The window's "Copy the report" gives what to send with a problem.

If the shaders' preparation hits a fault in the process's own memory (seen once in a while under Wine, never on Windows so
far), the run ends, the fault is written in its log, and the builder starts it again where it stopped; before 0.2.1 such a
fault could make it leave a program out, or wait for ever.

### Build it on Linux

The builder (`scripts/grapnel-builder/`) turns your game file into the console's title folder and data folder. On Linux
it is a command-line tool (the window of the Windows package runs there too: `scripts/grapnel-builder/grapnel_gui.py`),
modelled on the builder of
[heydemoura/PS5_Longshot](https://github.com/heydemoura/PS5_Longshot); its own guide,
[scripts/grapnel-builder/INSTALL.md](../scripts/grapnel-builder/INSTALL.md), has every step: install LLVM 19.1.7, git and
the builder's Python environment (`~/grapnel-venv`, from `scripts/grapnel-builder/requirements.txt`), clone this repository,
unpack the toolchain, then, in the repository's folder:

```bash
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --check --toolchain ~/grapnel-toolchain-0.3.3-linux-x64
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --nup "$HOME/<your game>.nup" --out ~/grapnel-out \
    --toolchain ~/grapnel-toolchain-0.3.3-linux-x64
```

- `--check` needs no game: it looks at everything the build needs (its Python packages, LLVM 19.1.7, the console
  toolchain: the PS5 payload SDK, PS5_Vulkan's linker parts, the RADV driver built from
  [ps5/mesa-patches](../ps5/mesa-patches/README.md), the renderer's headers, dav1d, at the revisions pinned in
  [ps5/DEPENDENCIES.md](../ps5/DEPENDENCIES.md)) and says what is missing.
- It reads the game's program out of the `.nup` (about 40 MB of it), translates it to C, compiles it for the PS5 with
  LLVM/clang 19.1.7 (the compiler every console test used: every player gets the same eboot), links and signs the title,
  and makes the controller's PlayStation icons from your game's own files.
- It prepares the game's shaders on your PC: the console's own shader preparation, run by the toolchain's `prepare/` (no
  game code: the console's renderer and a model of the console's driver built for the PC, so the console's cache keys) on
  every core, as many compiles at once as the free memory allows; then it checks the cache (a second pass rebuilds everything
  and finds it all) and puts it in the title folder (`radv-shader-cache/`). On Linux and Windows; `--no-prepare` leaves it
  out (the console then compiles in play). On a processor without AVX2 (x86-64-v3), which the preparation needs, it puts
  the shaders' lists and `HOOKSHOT_PS5_PREPARE=1` beside the game instead: the console prepares them at its first start.
- It refuses another game or another version than 131072, and a build that would leave the console too little memory.
- It takes about 45 minutes on a recent 8-core PC (the compile ~25, the shaders' preparation ~15), up to 2 hours and a half
  on a small one; with little memory add `--lift-jobs 1 --memory 4` (slower, never different). A new build after an update
  is much shorter.
- It writes `<output>/homebrew/GRPN00001/` (the title folder, with the prepared shaders in `radv-shader-cache/`),
  `<output>/data/grapnel/` (`image.bin`, the settings `hookshot_env.txt` and `skip.txt`, `prompts/`,
  `hookshot_prepare_progress.txt`, the launch screen's picture `hookshot_menu_fond.rgba`), the copying steps and
  `manifest.json`. Everything in the output contains the game: keep
  it to yourself.

### Install on the console

The window's step 6 does steps 1 and 2 over FTP (see [Build it on Windows](#build-it-on-windows)); by hand:

1. Copy `<output>/homebrew/GRPN00001/` (with its `radv-shader-cache/`) to **`/data/homebrew/GRPN00001/`**.
2. Copy `<output>/data/grapnel/` to **`/data/grapnel/`**, and your game file into it, as it is, as
   **`/data/grapnel/game.nup`**. **If the builder said your dump keeps game files compressed** (some dumpers do: the
   console cannot read those files, and the game would stop as Play is pressed), it made a flat copy of it, once, on your
   PC (`<output>/game.nup` by default, about 25.3 GB, checked file by file): copy **that copy**, not your dump. The
   window's step 6 sends it by itself; `<output>/INSTALL.en.txt` names the file. **Corrupted files detected**: the builder
   also compares your dump's files with a sound dump's (from the fingerprints the `.nup` holds) and names those your dump tool
   read wrong or left out; the game may miss a texture or a sound there, or stop: dump the game again if you can.
3. Register the title (ShadowMountPlus does it, or your usual homebrew tool; the title's `sce_sys/` must already hold its
   icon and pictures when it is registered: the home screen copies them then), then start **Grapnel** from the home screen.
4. Keep the ELF loader running: at start the title sends its storage broker to it (port 9021), which makes its large
   writes fast. Without it the log says `elf loader unavailable` and the title writes by itself, more slowly.

**On an extension disk** (ShadowMountPlus 1.7beta4 or newer): the title folder goes to `/mnt/ext1/homebrew/GRPN00001/` and
the game file to `/mnt/ext1/grapnel/game.nup`; `data/grapnel/` still goes to `/data/grapnel/`, and its `hookshot_env.txt`
gets the line `HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup`.

The settings file the builder puts in `/data/grapnel/` is the repository's
[`ps5/hookshot_env.default.txt`](../ps5/hookshot_env.default.txt) (with [`ps5/skip.txt`](../ps5/skip.txt) beside it): copy
it again as `/data/grapnel/hookshot_env.txt` to go back to the defaults.

**To update:** replace `eboot.bin` in the title folder and `image.bin` in `/data/grapnel/` with the new build's (the two go
together: a mismatched pair stops at start with `lift and image do NOT match`), and the driver's shader cache: delete
`/data/homebrew/GRPN00001/radv-shader-cache/` and copy the new build's, with `/data/grapnel/hookshot_prepare_progress.txt`.
Keep the rest: your saves (`user/`), options and caches in `/data/grapnel/`.
A folder `/data/hookshot` left by an earlier Hookshot PS5 build is renamed to `/data/grapnel` at the first start, saves
included.

### Settings and the launch screen

At start, Grapnel shows its **launch screen**: **Play**, **Settings**, **Shaders** (whether the shader caches are ready)
and **Quit**. The settings, saved at each change, are in five tabs:

- **Picture:** the game's frame rate (30 or unlocked), frame generation, the FPS counter;
- **Game:** text speed and size, skipping the opening, the game's language, the menus' language (automatic, English or
  French) and their size;
- **Controller:** the button layout (PlayStation or Nintendo), vibration, the gyroscope, the camera, the sticks' dead zone;
- **Sound:** the volume;
- **Launch:** show the screen **always**, or **when needed** (then hold **R1** while the title starts to see it; it also
  shows when the shader check finds a problem).

**The shaders.** The builder prepared them on your PC: at the first start the **Shaders** page says **Ready** (the driver's
cache in the title folder and the preparation's file beside the game, `hookshot_prepare_progress.txt`), and the console
prepares nothing. On a PC whose processor has no AVX2, the builder did not: it put the census `hookshot_shaders.bin` (made
from your game: private), `hookshot_pipeline_states_shipped.bin` and the line `HOOKSHOT_PS5_PREPARE=1` (at the end of
`hookshot_env.txt`) beside the game, and at the first start that page offers **Prepare the shaders**: about an hour on a
standard PS5, once (**Later** stops it, and the next start goes on where it stopped).

`/data/grapnel/hookshot_env.txt` holds the development settings the port is played and tested with (memory, driver,
picture). Keep the file as it is, and change a line only to try something; a setting it holds shows as such on the launch
screen. Two optional extras:

- **The log on your PC:** add `HOOKSHOT_PS5_LOG_UDP=<your PC's address>:9148` to the file and run
  `python3 -I scripts/ps5/udp_log.py` on the PC. The log is always in `/data/grapnel/hookshot_log.txt` too.
- **A background picture** for the launch screen: `python3 -I scripts/ps5/menu_fond.py <picture.png>
  /data/grapnel/hookshot_menu_fond.rgba [--panel X0,Y0,X1,Y1]` (made on the PC, then copied). A picture made by someone
  else is for your own console: do not publish it.

### The in-game menu

In the game, **touchpad + R1** opens and closes the Grapnel menu (Options stays the game's own pause). It shows the
settings that work on the console, **Back to the Grapnel screen** (closes the game and starts Grapnel again on its launch
screen) and **Quit**. In the menus the left stick moves like the D-pad.

**The ocarina can be played into the DualSense's microphone** (player 1's), when the game's own microphone option is on;
the menu also has Microphone, Microphone gain and Test microphone. The voice never leaves the console: nothing is sent or
recorded, nothing is heard while the game does not listen, and the controller's mute button cuts it. Tried on firmware
12.60; `HOOKSHOT_PS5_MIC=0` in `/data/grapnel/hookshot_env.txt` turns it off.

### Troubleshooting and reporting problems

- `elf loader unavailable` in the log: start the ELF loader on port 9021 before the title. Nothing is lost meanwhile.
- `lift and image do NOT match`: `eboot.bin` and `image.bin` come from different builds; copy both from the same one.
- The title closed itself: the end of `/data/grapnel/hookshot_log.txt` says why (low memory, most often when entering
  Castle Town or Kakariko after 20 minutes or more of play; see the known limits).

Report problems in **[PS5_Grapnel's issues](https://github.com/morpheus302/PS5_Grapnel/issues)**, not to Longshot or Magic
Bean: they do not support the PS5. Say which console and firmware, which ShadowMountPlus, what you did in the game, and
paste the end of `hookshot_log.txt` and the builder's `manifest.json`. **Never attach** the game, `eboot.bin`,
`image.bin`, `prompts/` or a cache.

### Building by hand

The builder is the reference. By hand: `RX_TARGET=ps5 python3 -I nx2/build.py --standalone` on a translated work folder,
with `PS5_PAYLOAD_SDK`, `PS5_VULKAN`, `RADV_ARCHIVE` (PS5_Mesa with [ps5/mesa-patches](../ps5/mesa-patches/README.md)),
`HOOKSHOT_PS5_DAV1D`, `RX_CPM` (`scripts/ps5/fetch-headers.sh`) and `HOOKSHOT_PS5_FLEXIBLE_MB=580`; the revisions are in
[ps5/DEPENDENCIES.md](../ps5/DEPENDENCIES.md). The port's design log of its first days is
[docs/ps5-port.md](../docs/ps5-port.md) (historical). The PC build and the menus are described in
[Longshot's README](../README.md), kept as it is.

### License

Grapnel is free software under the GNU General Public License, version 3 or later ([LICENSE](../LICENSE)), as Longshot and
Hookshot are. It is a modified version of Longshot (commit `ac778751` of 2026-11-16), itself a modified version of Hookshot:

    Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
    Longshot: Copyright (C) 2026 the Longshot contributors
    Grapnel:  Copyright (C) 2026 morpheus302 and the Grapnel contributors

The third-party parts and their licences are in [NOTICE.md](../NOTICE.md); what changed since Longshot, file by file and
dated, is in [docs/grapnel-changes.md](../docs/grapnel-changes.md).

### Credits

Longshot and Hookshot and everyone they credit; mihawk-99's PS5 stack (PS5_Mesa, PS5_Vulkan, PS5_PayloadSDK,
PS5_VulkanTemplate, PS5_MihawkGen); ps5-payload-dev's SDK; heydemoura's PS5_Longshot, the model for the builder;
bizkut; rimidalv333's Longshot Oberon (the lazy shader modules); Eden and suyu; Kenney (CC0); Dear ImGui and the other
parts. All of them, and what each brought: [CREDITS.md](../CREDITS.md).

### Not affiliated

Grapnel is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo or Sony Interactive
Entertainment. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo; PlayStation, PS5 and DualSense are
trademarks of Sony Interactive Entertainment Inc. Other names belong to their owners. This repository contains no game
code, game data or keys.

---

## Français

### Ce qu'est Grapnel

Grapnel porte sur PS5 [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot), la recompilation statique de la
version Switch 2 de *The Legend of Zelda: Ocarina of Time*. Le code du jeu est traduit en C et compilé pour le
processeur de la console ; le rendu est refait en Vulkan, par RADV pour PS5
([mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)) ; le logiciel système que le jeu attend est réécrit. Aucun
émulateur.

Tout se fait **sur votre PC, à partir de votre propre copie du jeu**. Ce dépôt ne contient ni code ni données du jeu, ni
clé, ni exécutable pour la console, et n'explique pas comment les obtenir. Ce que le constructeur fabrique à partir de
votre copie (`eboot.bin`, `image.bin`, les icônes de la manette, les caches) contient le jeu : c'est pour votre console
seulement, ne le partagez jamais.

Grapnel est publié avant d'être fini, pour que d'autres puissent aider.

### État et limites connues

- Il tourne sur une PS5 standard, firmware **12.60**, le seul essayé. **La PS5 Pro n'a jamais été essayée.**
- **Pas encore 30 images/s stables dans le lieu le plus lourd.** Sur la place du marché de la citadelle, cadence
  débloquée, le jeu tourne à environ 32 images par seconde, mais environ 15 à 25 % des images prennent plus de 34 ms : ça
  saccade à cet endroit. Ailleurs c'est plus fluide. Amener cette place à 30 stables est le principal travail restant.
- **Le premier lancement est le plus dur.** Tant que le cache de shaders du pilote n'est pas rempli, la console compile
  les programmes GPU du jeu pendant la partie : des saccades, et de la mémoire qui ne revient pas. Dans un lieu chargé, le
  titre peut alors se fermer de lui-même (ci-dessous). C'est pourquoi le constructeur **prépare les shaders sur votre PC**
  (environ 15 minutes) : la console les trouve dans le cache de son pilote dès le premier lancement et ne prépare rien ; seul un
  pipeline qu'aucune liste ne connaît est encore compilé en jouant. (Sur un PC sans AVX2, c'est la console qui les prépare
  elle-même avant la première partie, environ une heure.)
- **Une longue partie peut s'arrêter à un changement de lieu.** Au fil du jeu, la mémoire prise par le jeu grandit, et la
  rafale de textures chargées en entrant dans un lieu chargé (la citadelle depuis la plaine, le village Cocorico) peut en
  laisser trop peu : le titre se ferme alors de lui-même. Relancez-le : votre sauvegarde est telle que vous l'avez laissée.
  Depuis la 0.2.1, les shaders que la console lit dans son cache sont gardés compressés (modules de shaders paresseux et
  SPIR-V compressé libéré, repris de [Longshot Oberon](https://github.com/rimidalv333/longshot-oberon) de rimidalv333) :
  environ 290 Mo de mémoire libre en plus (leur SPIR-V tient en 105 Mo au lieu de 396), et la partie d'un joueur a duré
  **36 minutes sans fermeture**, contre 25 environ avant. Faire durer cette mémoire est l'autre grand chantier restant.
- Le titre surveille sa mémoire : quand la mémoire libre de la console devient trop basse, il se ferme et le dit dans son
  journal, plutôt que de laisser la console en manquer (les premières versions pouvaient éteindre toute la console).
- Les outils de Longshot réservés au PC (rapports de bogue, listes de pipelines) ne sont pas sur la console.

### Ce qu'il faut

- Une **PS5 débridée** (essayée en firmware 12.60 seulement) avec **kstuff**, un **chargeur d'ELF qui écoute sur le port
  9021** (elfldr) et **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) 1.7beta3 ou plus récent** (essayé :
  1.7beta11), qui rend `/data` visible au titre et l'enregistre ; **1.7beta4 ou plus récent** pour l'installer sur un disque
  d'extension (il rend alors ce disque visible au titre lui aussi).
- Votre réseau local entre le PC et la console : la fenêtre du constructeur peut envoyer le titre elle-même (étape 6, par
  FTP), ou vous le copiez vous-même (un serveur FTP, par exemple).
- **Votre propre copie du jeu**, extraite de votre propre console, en fichier `.nup` (version 131072 du jeu). Environ 26 Go.
- **Un PC sous Windows 10 ou 11 (64 bits), ou sous Linux x86-64**, pour le constructeur : 8 Go de mémoire au moins (16 Go,
  c'est bien plus rapide), environ 12 Go libres en plus du fichier du jeu. L'AVX2 n'est pas exigé : sur un processeur qui
  ne l'a pas (x86-64-v3 : bien des Intel Celeron, Pentium et Atom, les AMD anciens), la vérification le dit et le constructeur
  laisse les shaders à la console, qui les prépare elle-même à son premier lancement (environ une heure sur une PS5
  standard ; sur un tel PC, la préparation durerait à peu près autant). La construction le met en place d'elle-même.
  - **Windows :** `Grapnel-Builder-0.2.6-windows-x64.zip`, sur la page
    [Releases](https://github.com/morpheus302/PS5_Grapnel/releases) de ce dépôt : un dossier qui contient tout (son LLVM,
    Python, la chaîne d'outils console), rien à installer.
  - **Linux :** **LLVM 19.1.7**, git et Python 3.12 ou plus récent, et **la chaîne d'outils console**,
    `grapnel-toolchain-0.3.3-linux-x64.tar.xz`, sur la même page (le SDK, les pièces de lien et le pilote de la console, la
    préparation des shaders et le serveur FTP que l'étape 6 peut envoyer ; aucun fichier du jeu dedans).

### Construire sous Windows

1. Décompressez tout `Grapnel-Builder-0.2.6-windows-x64.zip` (clic droit, « Extraire tout… ») dans un dossier à vous au
   chemin court, par exemple `C:\Grapnel-Builder`. Rien à installer, pas de droits d'administrateur.
2. Lancez `Grapnel.exe`. Il n'est pas signé : si Windows affiche « Windows a protégé votre ordinateur », cliquez sur
   **Informations complémentaires**, puis **Exécuter quand même**. Un antivirus peut examiner ou bloquer les programmes du
   constructeur (`clang.exe`, `ld.lld.exe`, `zbic_unpack.exe`, `ps5-native-tool.exe`), qui compilent le jeu pour la
   console : s'il en retire un, l'étape 3 dit que les outils sont incomplets ; rétablissez-le ou excluez le dossier, puis
   décompressez à nouveau.
3. Suivez ses six étapes : **1** le `.nup` de votre jeu (contrôlé tout de suite), **2** le dossier de sortie (une
   construction précédente, là ou dans un autre dossier, est proposée), **3** la vérification de votre PC (toute seule),
   **4** la construction du jeu, **5** les shaders, préparés sur votre PC comme sous Linux, **6** l'envoi à la console
   (facultatif).
4. L'étape 6 demande l'adresse IP de la console. Elle envoie par FTP : le serveur FTP de la console s'il répond (port
   2121), sinon [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) (GPL-3.0), qu'elle envoie au chargeur d'ELF de la
   console (port 9021). Elle copie d'abord vos parties sur le PC (`console-saves\` dans le dossier de sortie), demande
   avant de remplacer quoi que ce soit, ne touche que le dossier du titre, celui du fichier du jeu et `/data/grapnel`,
   n'envoie le fichier du jeu que si la console ne l'a pas, reprend un envoi coupé là où il en était, et relit les tailles
   et le CRC32 de l'eboot. ShadowMountPlus enregistre ensuite le titre.
5. L'étape 6 installe sur le **stockage interne** de la console ou sur son **disque d'extension** (`/mnt/ext1`, trouvé tout
   seul). Sur le disque, le titre va dans `/mnt/ext1/homebrew/GRPN00001` et le fichier du jeu dans
   `/mnt/ext1/grapnel/game.nup` ; vos parties, réglages et journaux restent dans `/data/grapnel`, et le `hookshot_env.txt`
   envoyé là dit où est le fichier du jeu (`HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup`). Elle ne touche jamais aux autres
   jeux du disque. Un titre ou un fichier du jeu laissé à l'autre endroit n'est retiré que si vous dites oui, une fois
   l'envoi relu.

Sur un Ryzen 5 3600X avec 16 Go (le PC d'un joueur, Windows 11), deux constructions : la traduction 155 et 266 s, la
compilation 1 980 et 2 066 s (33 et 34 min), la préparation des shaders 1 318 et 2 839 s (22 et 47 min, contrôle compris). L'`eboot.bin` et le cache des shaders faits là sont ceux du
constructeur Linux, octet pour octet : le même CRC32 pour l'eboot, les mêmes 57 210 clés et contenus pour le cache.
`Grapnel-console.cmd` fait la même construction sans la fenêtre. « Copier le rapport » donne ce qu'il faut envoyer en cas
de problème.

Si la préparation des shaders rencontre une faute dans la mémoire du processus lui-même (vu de temps en temps sous Wine,
jamais sous Windows jusqu'ici), l'exécution s'arrête, la faute est écrite dans son journal, et le constructeur la relance là
où elle en était ; avant la 0.2.1, une telle faute pouvait lui faire laisser un programme de côté, ou attendre sans fin.

### Construire sous Linux

Le constructeur (`scripts/grapnel-builder/`) fait, à partir de votre fichier du jeu, le dossier du titre et le dossier de
données de la console. Sous Linux c'est un outil en ligne de commande (la fenêtre du paquet Windows y tourne aussi :
`scripts/grapnel-builder/grapnel_gui.py`), qui a pour modèle le constructeur
de [heydemoura/PS5_Longshot](https://github.com/heydemoura/PS5_Longshot) ; son guide,
[scripts/grapnel-builder/INSTALL.fr.md](../scripts/grapnel-builder/INSTALL.fr.md), donne chaque étape : installer LLVM
19.1.7, git et l'environnement Python du constructeur (`~/grapnel-venv`, depuis `scripts/grapnel-builder/requirements.txt`),
cloner ce dépôt, décompresser la chaîne d'outils, puis, dans le dossier du dépôt :

```bash
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --check --toolchain ~/grapnel-toolchain-0.3.3-linux-x64
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --nup "$HOME/<votre jeu>.nup" --out ~/grapnel-out \
    --toolchain ~/grapnel-toolchain-0.3.3-linux-x64
```

- `--check` n'a pas besoin du jeu : il vérifie tout ce dont la construction a besoin (ses paquets Python, LLVM 19.1.7, la
  chaîne console : le SDK de charges PS5, les pièces de lien de PS5_Vulkan, le pilote RADV construit avec
  [ps5/mesa-patches](../ps5/mesa-patches/README.md), les en-têtes du rendu, dav1d, aux révisions épinglées dans
  [ps5/DEPENDENCIES.md](../ps5/DEPENDENCIES.md)) et dit ce qui manque.
- Il lit le programme du jeu dans le `.nup` (environ 40 Mo), le traduit en C, le compile pour la PS5 avec LLVM/clang
  19.1.7 (le compilateur de tous les essais sur la console : chaque joueur obtient le même eboot), lie et signe le titre,
  et fait les icônes PlayStation de la manette à partir des fichiers de votre jeu.
- Il prépare les shaders du jeu sur votre PC : la préparation des shaders de la console elle-même, lancée par le `prepare/` de
  la chaîne d'outils (aucun code du jeu : le rendu de la console et un modèle du pilote de la console construit pour le PC, donc
  les clés de cache de la console) sur tous les cœurs, autant de compilations à la fois que la mémoire libre le permet ; puis
  il vérifie le cache (une seconde passe refait tout et trouve tout) et le met dans le dossier du titre (`radv-shader-cache/`).
  Sous Linux et sous Windows ; `--no-prepare` s'en passe (la console compile alors en jouant). Sur un processeur sans AVX2
  (x86-64-v3), dont la préparation a besoin, il met à la place les listes des shaders et `HOOKSHOT_PS5_PREPARE=1` à côté du
  jeu : la console les prépare à son premier lancement.
- Il refuse un autre jeu ou une autre version que 131072, et une construction qui laisserait trop peu de mémoire à la
  console.
- Il faut environ 45 minutes sur un PC récent à 8 cœurs (la compilation ~25, la préparation des shaders ~15), jusqu'à 2 heures
  et demie sur un petit ; avec peu de mémoire, ajoutez `--lift-jobs 1 --memory 4` (plus lent, jamais différent). Une nouvelle
  construction après une mise à jour est bien plus courte.
- Il écrit `<sortie>/homebrew/GRPN00001/` (le dossier du titre, avec les shaders préparés dans `radv-shader-cache/`),
  `<sortie>/data/grapnel/` (`image.bin`, les réglages `hookshot_env.txt` et `skip.txt`, `prompts/`,
  `hookshot_prepare_progress.txt`, l'image de l'écran de lancement `hookshot_menu_fond.rgba`), les étapes de copie et
  `manifest.json`. Tout ce qu'il écrit contient le jeu : gardez-le pour
  vous.

### Installer sur la console

L'étape 6 de la fenêtre fait les points 1 et 2 par FTP (voir [Construire sous Windows](#construire-sous-windows)) ; à la
main :

1. Copiez `<sortie>/homebrew/GRPN00001/` (avec son `radv-shader-cache/`) dans **`/data/homebrew/GRPN00001/`**.
2. Copiez `<sortie>/data/grapnel/` dans **`/data/grapnel/`**, et votre fichier du jeu dedans, tel quel, sous le nom
   **`/data/grapnel/game.nup`**. **Si le constructeur a dit que votre dump garde des fichiers du jeu compressés** (certains
   outils de dump le font : la console ne sait pas lire ces fichiers, et le jeu s'arrêterait en appuyant sur Jouer), il en
   a fait une copie à plat, une fois, sur votre PC (`<sortie>/game.nup` par défaut, environ 25,3 Go, vérifiée fichier par
   fichier) : copiez **cette copie**, pas votre dump. L'étape 6 de la fenêtre l'envoie d'elle-même ;
   `<sortie>/INSTALL.fr.txt` nomme le fichier. **Fichiers corrompus détectés** : le constructeur compare aussi les fichiers de
   votre dump à ceux d'un dump sain (d'après les empreintes que porte le `.nup`) et nomme ceux que votre outil de dump a mal lus
   ou oubliés ; le jeu peut y manquer d'une texture ou d'un son, ou s'arrêter : refaites le dump si vous le pouvez.
3. Enregistrez le titre (ShadowMountPlus le fait, ou votre outil homebrew habituel ; le `sce_sys/` du titre doit déjà
   contenir son icône et ses images à ce moment : l'accueil les copie alors), puis lancez **Grapnel** depuis l'accueil.
4. Laissez tourner le chargeur d'ELF : au démarrage, le titre lui envoie son courtier d'écriture (port 9021), qui rend ses
   grosses écritures rapides. Sans lui, le journal dit `elf loader unavailable` et le titre écrit lui-même, plus lentement.

**Sur un disque d'extension** (ShadowMountPlus 1.7beta4 ou plus récent) : le dossier du titre va dans
`/mnt/ext1/homebrew/GRPN00001/` et le fichier du jeu dans `/mnt/ext1/grapnel/game.nup` ; `data/grapnel/` va toujours dans
`/data/grapnel/`, et son `hookshot_env.txt` reçoit la ligne `HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup`.

Le fichier de réglages que le constructeur met dans `/data/grapnel/` est le
[`ps5/hookshot_env.default.txt`](../ps5/hookshot_env.default.txt) du dépôt (avec [`ps5/skip.txt`](../ps5/skip.txt) à côté) :
recopiez-le en `/data/grapnel/hookshot_env.txt` pour revenir aux réglages par défaut.

**Mettre à jour :** remplacez `eboot.bin` dans le dossier du titre et `image.bin` dans `/data/grapnel/` par ceux de la
nouvelle construction (les deux vont ensemble : une paire dépareillée s'arrête au démarrage avec `lift and image do NOT
match`), et le cache de shaders du pilote : effacez `/data/homebrew/GRPN00001/radv-shader-cache/` et copiez celui de la
nouvelle construction, avec `/data/grapnel/hookshot_prepare_progress.txt`. Gardez le reste : vos sauvegardes (`user/`),
options et caches dans `/data/grapnel/`. Un dossier `/data/hookshot` laissé par une ancienne version
Hookshot PS5 est renommé en `/data/grapnel` au premier lancement, sauvegardes comprises.

### Les réglages et l'écran de lancement

Au démarrage, Grapnel affiche son **écran de lancement** : **Jouer**, **Réglages**, **Shaders** (les caches de shaders
sont-ils prêts) et **Quitter**. Les réglages, enregistrés à chaque changement, sont en cinq onglets :

- **Image :** la cadence du jeu (30 ou débloquée), la génération d'images, le compteur d'images ;
- **Jeu :** la vitesse et la taille du texte, passer l'ouverture, la langue du jeu, la langue des menus (automatique, français
  ou anglais) et leur taille ;
- **Manette :** la disposition des boutons (PlayStation ou Nintendo), les vibrations, le gyroscope, la caméra, la zone
  morte des sticks ;
- **Son :** le volume ;
- **Lancement :** afficher l'écran **toujours**, ou **si nécessaire** (il faut alors tenir **R1** pendant le démarrage du
  titre pour le voir ; il s'affiche aussi quand la vérification des shaders trouve un problème).

**Les shaders.** Le constructeur les a préparés sur votre PC : dès le premier lancement, la page **Shaders** dit **Prêts** (le
cache du pilote dans le dossier du titre, et le fichier de la préparation à côté du jeu, `hookshot_prepare_progress.txt`), et la
console ne prépare rien. Sur un PC dont le processeur n'a pas l'AVX2, il ne l'a pas fait : il a mis à côté du jeu le recensement
`hookshot_shaders.bin` (fait à partir de votre jeu : privé), `hookshot_pipeline_states_shipped.bin` et la ligne
`HOOKSHOT_PS5_PREPARE=1` (à la fin de `hookshot_env.txt`), et dès le premier lancement cette page propose **Préparer les
shaders** : environ une heure sur une PS5 standard, une seule fois (**Plus tard** l'arrête, et le lancement suivant reprend où
elle en était).

`/data/grapnel/hookshot_env.txt` contient les réglages de développement avec lesquels le portage est joué et essayé
(mémoire, pilote, image). Gardez le fichier tel quel, et ne changez une ligne que pour essayer ; un réglage qu'il fixe est
signalé comme tel sur l'écran de lancement. Deux options :

- **Le journal sur votre PC :** ajoutez `HOOKSHOT_PS5_LOG_UDP=<adresse de votre PC>:9148` au fichier et lancez
  `python3 -I scripts/ps5/udp_log.py` sur le PC. Le journal est de toute façon dans `/data/grapnel/hookshot_log.txt`.
- **Une image de fond** pour l'écran de lancement : `python3 -I scripts/ps5/menu_fond.py <image.png>
  /data/grapnel/hookshot_menu_fond.rgba [--panel X0,Y0,X1,Y1]` (faite sur le PC, puis copiée). Une image faite par
  quelqu'un d'autre est pour votre console seulement : ne la publiez pas.

### Le menu en jeu

En jeu, **pavé tactile + R1** ouvre et ferme le menu Grapnel (Options reste la pause du jeu). Il montre les réglages qui
marchent sur la console, **Revenir à l'écran Grapnel** (ferme le jeu et relance Grapnel sur son écran de lancement) et
**Quitter**. Dans les menus, le stick gauche se déplace comme la croix.

**L'ocarina peut se jouer au micro de la DualSense** (celle du joueur 1), quand l'option micro du jeu est activée ; le menu
a aussi Microphone, Gain du microphone et Tester le microphone. La voix ne quitte jamais la console : rien n'est envoyé ni
enregistré, rien n'est capté tant que le jeu n'écoute pas, et le bouton muet de la manette coupe tout. Essayé en firmware
12.60 ; `HOOKSHOT_PS5_MIC=0` dans `/data/grapnel/hookshot_env.txt` l'éteint.

### En cas de problème

- `elf loader unavailable` dans le journal : lancez le chargeur d'ELF sur le port 9021 avant le titre. Rien n'est perdu
  entre-temps.
- `lift and image do NOT match` : `eboot.bin` et `image.bin` viennent de deux constructions ; copiez les deux de la même.
- Le titre s'est fermé de lui-même : la fin de `/data/grapnel/hookshot_log.txt` dit pourquoi (mémoire basse, le plus
  souvent en entrant dans la citadelle ou à Cocorico après 20 minutes de jeu ou plus ; voir les limites connues).

Signalez les problèmes dans les **[issues de PS5_Grapnel](https://github.com/morpheus302/PS5_Grapnel/issues)**, pas chez
Longshot ni chez Magic Bean : ils ne s'occupent pas de la PS5. Dites quelle console et quel firmware, quel ShadowMountPlus,
ce que vous faisiez dans le jeu, et collez la fin de `hookshot_log.txt` et le `manifest.json` du constructeur. **Ne joignez
jamais** le jeu, `eboot.bin`, `image.bin`, `prompts/` ni un cache.

### Construire à la main

Le constructeur fait référence. À la main : `RX_TARGET=ps5 python3 -I nx2/build.py --standalone` sur un dossier de
travail traduit, avec `PS5_PAYLOAD_SDK`, `PS5_VULKAN`, `RADV_ARCHIVE` (PS5_Mesa avec
[ps5/mesa-patches](../ps5/mesa-patches/README.md)), `HOOKSHOT_PS5_DAV1D`, `RX_CPM` (`scripts/ps5/fetch-headers.sh`) et
`HOOKSHOT_PS5_FLEXIBLE_MB=580` ; les révisions sont dans [ps5/DEPENDENCIES.md](../ps5/DEPENDENCIES.md). Le journal de
conception des premiers jours du portage est [docs/ps5-port.md](../docs/ps5-port.md) (historique). La construction pour
PC et les menus sont décrits dans [le README de Longshot](../README.md), gardé tel quel.

### Licence

Grapnel est un logiciel libre sous la licence publique générale GNU, version 3 ou ultérieure ([LICENSE](../LICENSE)), comme
Longshot et Hookshot. C'est une version modifiée de Longshot (commit `ac778751` du 16/11/2026), lui-même version modifiée
de Hookshot :

    Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
    Longshot: Copyright (C) 2026 the Longshot contributors
    Grapnel:  Copyright (C) 2026 morpheus302 and the Grapnel contributors

Les composants tiers et leurs licences sont dans [NOTICE.md](../NOTICE.md) ; ce qui a changé depuis Longshot, fichier par
fichier et daté, est dans [docs/grapnel-changes.md](../docs/grapnel-changes.md).

### Crédits

Longshot et Hookshot, et tous ceux qu'ils citent ; la pile PS5 de mihawk-99 (PS5_Mesa, PS5_Vulkan, PS5_PayloadSDK,
PS5_VulkanTemplate, PS5_MihawkGen) ; le SDK de ps5-payload-dev ; PS5_Longshot de heydemoura, modèle du constructeur ;
bizkut ; Longshot Oberon de rimidalv333 (les modules de shaders paresseux) ; Eden et suyu ; Kenney (CC0) ; Dear ImGui et
les autres composants. Tous, et ce que chacun a apporté :
[CREDITS.md](../CREDITS.md).

### Non-affiliation

Grapnel est un projet de fans non officiel, sans lien avec Nintendo ni Sony Interactive Entertainment, qui ne
l'approuvent ni ne le soutiennent. The Legend of Zelda et Ocarina of Time sont des marques de Nintendo ; PlayStation, PS5
et DualSense sont des marques de Sony Interactive Entertainment Inc. Les autres noms appartiennent à leurs propriétaires.
Ce dépôt ne contient ni code ni données du jeu, ni clé.
