# The PS5 port: the platform layer

> **Historical: the design log of 2026-11-09**, kept because it is the most complete technical description of the port's
> platform layer. It was written before the console first ran the game, and much has changed since: the title is now
> `GRPN00001` "Grapnel", its data folder `/data/grapnel`, and most of what it plans has been done or redone. The current
> state is in [Grapnel's README](../.github/README.md). Its references to `hookshot-ps5/`, `PLAN.md`, agents and internal
> branches name the author's working notes and branches, which are not published ([NOTICE.md](../NOTICE.md)).

Branch `ps5-host`, from upstream `8b2e1d3` (v2.0.7). Phase 3 of `hookshot-ps5/PLAN.md` (the runtime compiled for the
console, no game run) and the design phase 4 builds on. Nothing here has run on the console yet: every PS5 code
path compiles and is archived, and each one says what it rests on and what phase 4 must prove.

The GPU side (`gpu/vk`, Eden, sirit: branch `ps5-gpu`) is another agent's. This branch owns `nx2/build.py`,
`nx2/toolchain_ps5.py` and `host/`, and changes nothing under `gpu/`. The few lines `gpu/vk` needs are listed in
"Coordination" at the end.

## 1. Where phase 3 stands (2026-11-09)

| Piece | State |
|---|---|
| `RX_TARGET=ps5` | `nx2/build.py` and `nx2/toolchain_ps5.py`: the payload SDK's `prospero-clang`, `-march=x86-64-v3 -mtune=znver2` forced, static archives, the eboot's link sketched |
| `host/` + `host/posix` | **compiles for the console, 0 errors**, with `-Werror=implicit-function-declaration -Werror=int-conversion` on top of upstream's flags: 35 objects |
| a64-kit's runtime (`rt64/a64_rt.c`) | compiles |
| `third_party/opus` | compiles: 137 objects |
| `third_party/zbic` | compiles: 2 objects (a build-time tool on the PC; built for completeness) |
| One lifted chunk (`code_012.c`, 23 MB) | **compiles, 0 errors, 72 s, 1.5 GB peak**; the same chunk for Linux on the same PC: 76 s, 1.5 GB |
| Linux build | the changed files' Linux objects are byte-identical (`.text`) to `8b2e1d3` |

Archives (`work/sa/build-ps5/ps5/`):

| Archive | Objects | Size |
|---|---|---|
| `libhookshot_host.a` | 35 | 1.2 MB |
| `libhookshot_rt.a` | 1 | 14 KB |
| `libopus.a` | 137 | 0.8 MB |
| `libzbic.a` | 2 | 0.8 MB |
| `libhookshot_game.a` | 1 (the test chunk) | 10.5 MB |
| `libhookshot_vk.a` | the GPU side's | |

### Building

```sh
# the runtime alone (host/, a64-kit's runtime, opus, zbic): no game code is compiled
RX_TARGET=ps5 nice -n 19 .venv/bin/python -I nx2/build.py --runtime --jobs 3
# one chunk of the lifted game (the lift's work/sa/gen must be there)
RX_TARGET=ps5 nice -n 19 .venv/bin/python -I nx2/build.py --standalone --only 12 --jobs 1
# the whole game, the link plan printed (phase 5)
RX_TARGET=ps5 .venv/bin/python -I nx2/build.py --standalone --jobs 4
# what the archives import, and who answers it on the console
RX_TARGET=ps5 .venv/bin/python -I nx2/toolchain_ps5.py surface work/sa/build-ps5/ps5/*.a --others <RADV archive>
```

The SDK is found from `PS5_PAYLOAD_SDK`, `HOOKSHOT_PS5_SDK`, or the first
`ps5-stack/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk` above the repository; PS5_Vulkan (the RADV archive, the
link recipe) from `PS5_VULKAN` or `ps5-stack/PS5_Vulkan`. Objects go to `<work>/sa/build-ps5/`, apart from the PC's
`build/`, so the two targets never rebuild each other's objects.

### The one-chunk test

`code_012.c` from the PC build's lift (`~/.cache/hookshot-builder/sa/gen`, linked into `work/sa/gen`, never copied),
one compiler, `nice -n 19`, the machine shared with three other builds:

| | PS5 (`prospero-clang`, clang 19.1.7) | Linux (clang 19.1.7, same flags) | PC build log (14 jobs) |
|---|---|---|---|
| Errors | 0 | 0 | 0 |
| Wall / user time | 72.1 s / 71.2 s | 76.1 s / 75.4 s | 59 s |
| Peak memory | 1541 MB | 1531 MB | |
| Object | 10.4 MB | 10.6 MB | 10.1 MB |

- **No lifted code uses thread-local storage** (no `__emutls_*` symbol in the object): the console's emulated TLS
  costs the lifted code nothing.
- **Calls go through the GOT at compile time.** The target's `-fno-plt` makes each call to another chunk's function
  `call *f@GOTPCREL(%rip)` (26,873 `R_X86_64_GOTPCRELX` in this chunk; Linux has `PLT32` direct calls). They are
  relaxable: `prospero-lld -pie` turned them into `addr32 call f` and the data loads into `lea` in a two-file test,
  so the eboot keeps direct calls. Phase 5 checks the real link the same way.
- **Frame pointers are on** for this target at `-O1` (`push %rbp; mov %rsp,%rbp`), so console backtraces walk
  through lifted frames.
- Whole game: the PC's per-file seconds add up to 2.7 CPU-hours (14 jobs, 16 min); the PS5 target is not slower,
  so about an hour at 3 jobs, or the PC's 16 minutes with the memory for 14.

## 1b. Phase 4 on branch `ps5-plat` (2026-11-09): the platform layer, and a probe title to prove it

Branch `ps5-plat` (from `ps5-host`) changes host/posix's PS5 files and adds four, with one-line hooks elsewhere
(`host/mkw_glue.c` commit_on_touch, `host/hos.h` HOS_SPACE_END, `host/posix/win32_posix.c` one include,
`nx2/build.py` the file list). The probe title ran on the console twice (results below); the window's and the fibers' code
also run on the PC against the platform's kernel model (`scripts/ps5/build-plat-probe.sh host-test`: 21 checks pass).

| Part | Files | What changed |
|---|---|---|
| Guest window | `host/posix/ps5_window.c` (new) | Its own backing, not `ps5_vrange_commit`: a 64 KiB unit per direct-memory allocation, a bit per unit, the units' starts in a table per GiB mapped from direct memory by system calls, a spinlock per GiB held only across system calls. Nothing in the fault path locks a mutex, allocates from a heap or logs through stdio (risk M5). A fault backs the 1 MiB around it (`HOOKSHOT_PS5_FAULT_KB`). A unit already backed is never mapped or protected again (an import's GPU access survives a commit: risk M15). Decommit lays a reservation over the unit (no free moment). The reservation: one 512 GiB range, else 32 adjacent pieces of 16 GiB (same addresses), else the pieces granted from the base up when they reach 336 GiB, the guest then told a shorter address space (`hos_space_end`) |
| Fault hook | `host/mkw_glue.c` | commit_on_touch calls `hos_ps5_guest_fault` on the console (no hos_log, no VirtualQuery there) |
| Space end | `host/hos.h` | `HOS_SPACE_END` is `hos_space_end` on the console, as on the web build (512 GiB unless the window is shorter) |
| Futexes | `host/posix/ps5_platform.c` | `_umtx_op` refused (ENOSYS, EPERM, EINVAL, EFAULT) switches to condition variables by address, said once (T7) |
| /data | `host/posix/ps5_platform.c` | `hos_ps5_data_init` in `hos_ps5_start`, before klog's thread: `/data/hookshot` when the sandbox has `/data` (ShadowMountPlus 1.7beta3+), Lapy only when `/app0/hookshot-elevate` exists; the window's state logged at start |
| Open files | `host/posix/ps5_files.c`, `ps5_fdv_hooks.h` (new) | win32_posix.c's open/close/read/pread/write/pwrite/lseek/fstat/ftruncate/fdatasync through `ps5platform/fdv.h` (F9), with the number holder an AF_INET socket and fdv's /dev/null the eboot when the sandbox refuses theirs; writes of 16 KiB or more to files under /data/hookshot made by the storage broker when it runs |
| stdio's writes | `host/posix/ps5_stdio.c` (new) | `--wrap=fopen --wrap=fwrite` in the eboot's link: fwrite of 16 KiB or more to a stream opened for writing under /data/hookshot made by the broker at the stream's position (the renderer's caches) |
| Fibers | `host/posix/ps5_fiber.c` (new), `win32_posix.c` | an own context switch in place of getcontext/makecontext/swapcontext, which corrupted the heap on the console (T13) |
| Storage broker | `host/posix/ps5_broker.h`, `ps5_broker_client.c`, `ps5_broker_payload.c` (new) | Adapted from Forever Launcher's write broker (bizkut/ps5-forever `0b6abf0`, GPL-3.0): a payload the title sends to the ELF loader (127.0.0.1:9021), on 127.0.0.1:9147, token in `/data/hookshot` and the title folder; writes, reads, create, mkdir, stat, statfs, remove under `/data/hookshot`. Not wired into WriteFile yet |
| Pad, sound | `host/posix/hos_sdl_ps5.c` | scePad (a player per signed-in user, `PAD_MAP` to SDL's positions, sticks and triggers to SDL's ranges, rumble) and sceAudioOut (48 kHz S16 stereo, a thread pulling waveOut's queue), after PS5_VulkanTemplate's `ps5/src/platform.c`. `hos_sdl_init(HOS_SDL_VIDEO)` still answers 0 (D1: no surface yet), so display_sdl.c stays headless |

**The probe title, PPSA99147 "Hookshot Platform Probe"** (`scripts/ps5/plat-probe`, `scripts/ps5/build-plat-probe.sh`;
`dist/PPSA99147`, copied to `hookshot-ps5/probe/dist/plat-probe/PPSA99147`; `EXPECTED.txt` there has the steps and what
each line should say). It links the objects nx2/build.py makes for the eboot (ps5_platform, ps5_window, ps5_files,
ps5_broker_client, hos_sdl_ps5, win32_posix, mkw_glue, pad_sdl, a64-kit's runtime) with PS5_Vulkan's stock RADV, and runs:
the address space (every mapping to 1 TiB, the 16 GiB slots, where the map ends, the window's fallbacks), the 128 MiB
`.bss` table against flexible memory, futexes, the pad and a tone, backing at once and on first touch from 8 threads
under a watchdog, stacks and MXCSR, RADV's import of unit-backed memory with a compute shader, guest memory in 256 MiB
steps up to 9 GiB beside RADV's device, 320 files through CreateFileA, `/data/hookshot` written by the title past the
1.3 GiB burst, and the broker (2 GiB written, read back).

**The first console data** (agent A's boot test PPSA99145, `logs/ps5-boot-run1`, built from `ps5-host`): one 512 GiB
reservation at 0x80_0000_0000 is refused; a 64 MiB-stack thread runs with MXCSR 0x1f80; 12,244 MB of direct memory free
in one range; 268 MB of flexible memory free (403 MiB is usual at a title's start: the 128 MiB table looks charged at
load, the probe's `bss` check says). The likeliest reason for the refusal: the title's map ends at 0xFC_0000_0000, as the
PS4's application area does (EglantinePS5 reserves 256 GiB at 0xA0_0000_0000; RADV's winsys owns
[0x40_0000_0000, 0x80_0000_0000)), 16 GiB short of the window's end. ps5_window.c's prefix fallback then runs the game
with a 496 GiB address space; the clean fix, once `window.top` confirms it, is `A64_HOST_BASE` 0x7C_0000_0000
(`nx2/toolchain_ps5.py`, one constant, everything rebuilt), which keeps the PC's layout.

### The probe's console runs (2026-11-09, firmware 12.60, `logs/plat-probe`)

(The probe's first line says "system software 13.090.001": what `sceKernelGetSystemSwVersion` answered the title,
not the console's firmware, which was 12.60, the only one Grapnel has run on.)

| Check | Run 1 (all) / run 2 (pad, vk, memory, files, data) |
|---|---|
| window | the title's map ends at 0xFC_0000_0000 (the last 16 GiB slot refused; 64 KiB bisection); 496 GiB at 0x80_0000_0000 granted as 31 pieces of 16 GiB: plan C', `hos_space_end` 496 GiB. 512 GiB at 0x7C_0000_0000 is granted too, but needs every object rebuilt and overlaps RADV's range: not taken |
| bss | the 128 MiB table is charged at load: 285 MiB of flexible memory free at main, of 448 |
| futex | `_umtx_op` works (WaitOnAddress 50.1 ms timeout, 20.1 ms wake); the fallback works too |
| eager, lazy | 256 MiB backed at once at ~7 GiB/s; first touch 139 us a 1 MiB fault; 8 threads at once, interleaved, with locks held: no deadlock |
| stacks | 64 MiB-stack threads in direct memory, 7 MiB of recursion, 40 at once with flexible memory unchanged. MXCSR control 0x1f80 everywhere; the main thread's 0x1fa2 is 0x1f80 plus its own sticky precision and denormal flags. Run 1 crashed in DeleteFiber's free after one ucontext fiber: fibers now switch on `ps5_fiber.c` |
| pad, tone | scePad presses and sticks with the HOS_PAD and Switch names, rumble; 1 s of 440 Hz through waveOut, 191 grains |
| vk | 64 MiB of 1,024 units imported and written by a compute shader, a commit across it while imported, a first-touch range: every word right |
| memory | 9,216 MiB of guest memory held beside RADV's device (the target), all given back |
| files | libc stops at 249; fdv did not start (run 2), and run 3 found why: a title may not dup2 over a file (EPERM). fdv is off; the RomFS is one descriptor (game.nup, `host/nup_fs.c`) |
| pad (runs 3, 4) | an unattended controller flapped "disconnected"/"connected" once a second (46 times, run 3), a held one never (run 4): handles now kept, presence from samples with a 2 s grace and scePadReadState when nothing is new |
| broker.writefile (run 3) | every write fell back: CreateFileA made files 0644 and the broker is another identity. Files under /data are now 0666, folders 0777 |
| data | `/data` visible to the title (ShadowMount+ 1.7beta11): the data root is `/data/hookshot`. The title's own writes: 243 MiB/s for ~1 GiB, then 16 MiB/s; reads ~1.7-1.9 GiB/s |
| broker | started through the ELF loader in 0.9 s; 2 GiB at 151 MiB/s sustained; reads right. WriteFile and stdio under `/data/hookshot` now go through it |

## 2. Key decisions

1. **The guest window moves to 0x80_0000_0000** (`A64_HOST_BASE`, given by `nx2/toolchain_ps5.py` to every
   compile). Linux and Windows put it at 16 TiB; the console grants nothing at or above 1 TiB (PROBE.md, "Virtual
   space"). 512 GiB at 512 GiB ends exactly at 1 TiB, above the platform heap (0x20_0000_0000, 16 GiB) and RADV's
   device memory (0x40_0000_0000). Lifted code still has no base to load: a guest address is `0x8000000000 + addr`.
2. **The Win32-on-POSIX shim is the platform layer.** `host/posix/win32_posix.c` keeps one shape for Linux and the
   PS5; its `__PROSPERO__` branches call `host/posix/ps5.h` (`ps5_platform.c`), which calls the SDK fork's
   `ps5platform`. `host/*.c` changes are three files (`hos_main.c`, `mkw_glue.c`, `windows.h`'s one define).
3. **SDL's interface stays, its body changes.** `host/hos_sdl.c` (SDL loaded at run time) is replaced on the
   console by `host/posix/hos_sdl_ps5.c`, the same `hos_sdl.h` on VideoOut, scePad and sceAudioOut. The window loop
   (`display_sdl.c`), the controllers (`pad_sdl.c`) and the sound (`win32_posix.c`'s waveOut) are Linux's as they
   are.
4. **No dlopen: a table of linked entry points.** `LoadLibraryA`/`GetProcAddress` answer `hookshot_vk`
   (`mkwvk_init`) and `vulkan-1` (`vkGetInstanceProcAddr` = RADV's `vk_icdGetInstanceProcAddr`). The renderer's
   own Vulkan loading (`gpu/vk/vk_main.cpp:33`) and the runtime's (`host/rx_backend.c:231`) need no change.
5. **Backing is physical, so it is lazy.** Windows' commit charges a page file and Linux maps on first touch; on
   the console each 64 KiB of the window is a direct-memory allocation. Small hand-outs are backed at once, large
   ones (the heap, big address regions) at first touch, from the SIGSEGV handler that Windows' commit-on-touch
   already is.
6. **No write watch is needed.** The soft write watch (`A64_WRITE_TRACK`, on by default) marks pages from the lifted
   stores and from host code, as on Linux. The console's 16 KiB pages do not matter to it.
7. **Every stack is placed in direct memory by us.** The platform's `pthread_create` wrap gives 2 MiB only to
   threads that ask for less; a larger stack comes from the title's 448 MiB of flexible memory. Guest threads ask
   64 MiB: reserved at 64 MiB, backed at the top 8 MiB.
8. **The game folder is a root.** `GetModuleFileNameA` answers `<root>/eboot.bin`, so every "beside the executable"
   path (image.bin, romfs/, user/, the options, the logs, the caches) lands in the root: `/data/hookshot` once `/data`
   is granted, else `/app0`, the title's folder.
9. **No FFmpeg yet; the movies are skipped** behind a header shim, as on a Linux build without dav1d.

## 3. Memory

### Today

| What | Where |
|---|---|
| Window base, size | `third_party/a64-kit/rt/a64.h:61` (`A64_HOST_BASE` 16 TiB), `host/hos.h:45-46` (`HOS_HEAP_BASE` 64 GiB, `HOS_GUEST_SPACE` 512 GiB), `host/hos.h:68` (`HOS_SPACE_END`) |
| Layout told to the game | `host/hos_svc.c:24-33` (svcGetInfo's alias, heap, ASLR regions) |
| Reservation | `host/hos_main.c:122` `reserve_guest`: Windows `VirtualAlloc(MEM_RESERVE)`, Linux one `mmap(MAP_NORESERVE)` (a memfd for udmabuf) |
| Commit | `host/mkw_glue.c:18` `hos_commit`, called by the image load (`hos_main.c:1304`), `hos_mem_init`, `hos_region_alloc` (`hos_mem.c:114`), `SetMemoryHeapSize` (`hos_mem.c:203`), `AllocateMemoryPages` (`hos_mem.c:242`) |
| Commit on first touch | `host/mkw_glue.c:63` `commit_on_touch` (64 KiB at a time), installed by `hos_commit_init` (`mkw_glue.c:214`) |
| Decommit | `host/hos_mem.c:126` `discard_pages`, `FreeMemoryPages` (`hos_mem.c:283`), Linux's `hos_posix_guest_discard` (`MADV_REMOVE`/`DONTNEED`) |
| Write watch | the soft one: `nx2/rtgen.py:32` (`A64_DIRTY_PAGES`, a byte per 4 KiB page: 128 MiB), `rtgen.py:49` (`A64_MARK`), `nx2/build.py:383` (`-DA64_WRITE_TRACK`), `host/hos_mem.c:174` (`hos_mark_written`), read by `host/rx_backend.c:77` |
| The GPU's view | `gpu/vk/vk_memory.cpp:353` commits reserved pages before a host-pointer import; `vk_memory.cpp:358` udmabuf (`__linux__` only) |
| Memory the game is told | `host/hos_mem.c:51`: 9809 MB, `HOS_MEMORY_MB`; used: program + heap + the pages it mapped (`hos_used_memory`, from longshot `a4c77174`; `HOS_MEMINFO_OLD=1`: as before; `HOS_PAGES_LIMIT=1`: `AllocateMemoryPages` refused past the total, a diagnostic) |

### On the console

(Branch `ps5-plat` replaces the backing described here, `ps5_vrange_commit`, by its own in `host/posix/ps5_window.c`, and
adds the reservation's fallbacks: section 1b.)

- **Reservation** (`hos_main.c:144`, `host/posix/ps5_platform.c:48`): `ps5_vrange_reserve_at(0x8000000000, 512 GiB)`,
  made first in `main()` (`hos_main.c:1423`), before klog's thread or any stack is placed: an unplaced reservation
  takes the lowest free 4 GiB step up to 1 TiB and could take the window's place. `reserve_guest` finds it made.
- **Backing** (`ps5_platform.c:71`): `ps5_vrange_commit`, direct memory in 64 KiB units, zeroed, 16 KiB pages for the
  protection. A bit per unit (`ps5_platform.c:46`, 1 MiB) answers `VirtualQuery` without a kernel call
  (`win32_posix.c:807`), so `commit_on_touch` and `gpu/vk/vk_memory.cpp:353` work as on Windows.
- **What is backed when** (`mkw_glue.c:18`): ranges up to `HOOKSHOT_PS5_EAGER_MB` (256) at once (the image's
  `fread`, the system region, small regions: the kernel answers EFAULT on an unbacked page, it raises no fault);
  larger ones at first touch. `ReadFile` and `WriteFile` back their guest buffers before the kernel call
  (`win32_posix.c:1120`, `:1136`).
- **Decommit** (`hos_main.c:224`): `ps5_vrange_decommit`; whole units go back to the pool and read as zero at their
  next commit, partial ones are zeroed and kept. Horizon's 4 KiB semantics hold; the console just keeps more.
- **Write watch**: nothing to replace (decision 6). Its cost is the table: see "The 128 MiB table" below.
- **16 KiB pages**: nothing in production protects guest pages. The debugging page watches of `nvn_native.c`
  round to 16 KiB (`VirtualProtect`, `win32_posix.c`).
- **Other reservations** (`VirtualAlloc` outside the window, fiber and thread stacks): `ps5_vrange_reserve` +
  `ps5_vrange_commit`, placed, never the GPU window, never flexible memory.
- **malloc**: every `malloc` of host/ and gpu/ goes to the platform heap (direct memory) through the link
  recipe's `--wrap` (phase 5's link, nothing in the code).

### Phase 4 must prove, in this order

1. **The 512 GiB reservation.** Measured so far: 16 GiB at a hint, up to 0x80_0000_0000. If one 512 GiB range is
   refused, plan B: reserve the window's three used parts apart (the system region and image below 4 GiB; the heap
   up from 64 GiB; the address regions down from 512 GiB) and leave the rest unmapped. Plan C: a smaller window,
   `HOS_SPACE_END` and `HOS_GUEST_SPACE` lowered on the console (everything the game is told derives from them,
   `hos_svc.c:24-33`), with `A64_DIRTY_PAGES` to match (`nx2/rtgen.py`).
2. **Commit from the signal handler.** `ps5_vrange_commit` takes the platform's `units_lock` and may `calloc` to grow
   its unit table (`PS5_PayloadSDK platform/src/shm.c`). A fault inside the heap's own lock would deadlock. Size
   the table at start (one large commit and decommit) and back 1 MiB per fault rather than 64 KiB (PROBE.md: ~57 us
   a fault, ~30 us a commit: 8 GiB touched 64 KiB at a time is ~12 s of faults over a session).
3. **RADV's host-pointer import of these pages.** The smoke title imported application memory; not memory mapped
   unit by unit through `ps5_vrange_commit`. Test one import of a 16 MiB pool made of 256 units.
4. **The budget** (PLAN.md gap 5, the blocking risk): `ps5_memory_query` at start, after boot, in Kokiri;
   `HOS_MEMORY_MB` lowered from 9809 MB; `kernel.flexibleMemorySize` (up to 1 GiB, taken from direct memory) only
   if flexible memory runs short.

### If the window moves or shrinks (the first console run refused 512 GiB at 0x80_0000_0000)

What each answer costs, and where it lands (branch `ps5-port`):

- **Another base** (`A64_HOST_BASE`): one constant, `nx2/toolchain_ps5.py` `GUEST_BASE`, given to every compile. Every
  lifted access is `A64_MEM + addr` with the base an immediate, so every object key changes: the 80 lifted files again
  (about an hour at 3 jobs, the eight `code_hot_*.c` the longest), host/ and the renderer (minutes), then both links
  (`--runtime`, `--standalone`: under a minute each). Nothing else names it: not the link, the linker script,
  `param.json`, nor the boot test (stage 2 prints it, stage 8 derives its probe from it). The base must stay below
  1 TiB, the window clear of the platform heap (0x20_0000_0000, 16 GiB), RADV's device memory (0x40_0000_0000) and what
  `ps5_vrange_reserve` places below (the boot test's 64 MiB thread stack was at 0x4_0420_ff5f), reserved first in `main`.
- **A smaller window** (plan C: `HOS_SPACE_END`, `HOS_GUEST_SPACE` in `host/hos.h`, under `__PROSPERO__`; the game is
  told the layout from them, `hos_svc.c`; the renderer asks the host, `guest_size`): host/ only (35 files, under a
  minute) and the two links. The lifted chunks and the renderer stay: the chunks' keys are the lift's and `rt64`'s
  headers only. The soft write watch's table (`nx2/rtgen.py` `A64_DIRTY_PAGES`, 128 MiB for 512 GiB) masks addresses into
  itself and can stay as it is; shrinking it rebuilds the chunks. What the lift fixes: the system region
  (0x0100_0000-0x0700_0000) and the image (0x8000_0000-0x8602_0000, `image.bin`'s 100,794,368 bytes); the heap base
  (`HOS_HEAP_BASE`, 64 GiB) and the regions' top (`HOS_SPACE_END`) are the host's choices, so a window of
  64 GiB + the heap (`HOS_MEMORY_MB`) + the regions is the floor unless `HOS_HEAP_BASE` comes down too.
- **The parts reserved apart** (plan B): `host/posix/ps5_platform.c` only (`hos_ps5_guest_reserve`, `hos_ps5_in_guest`,
  the unit map), plus `main`'s two calls (`host/hos_main.c`: the first try, and the second after klog) if its signature
  changes. No other rebuild than host/.

### The 128 MiB table

The soft write watch's `a64_dirty` is a byte per 4 KiB page of the window: 128 MiB of `.bss` (the PC binary's
`.bss` is 157 MiB in all). Whether an eboot's `.bss` is charged to flexible memory (448 MiB) is not known: measure
`sceKernelAvailableFlexibleMemorySize` before and after the first marks. If it is, the choices are a smaller window
(plan C shrinks the table with it), a mark per 16 KiB (32 MiB, coarser uploads), or the table at a fixed address in a
direct-memory reservation (`--defsym`; each mark then loads its base from the GOT).

## 4. Threads, the CPU, floating point, TLS

| What | Where | On the console |
|---|---|---|
| Guest threads | `host/hos_thread.c:471`: `CreateThread(64 MiB)` | `win32_posix.c:257`: the stack placed by us (`hos_ps5_stack`, `ps5_platform.c:111`): 64 MiB reserved, the top `HOOKSHOT_PS5_STACK_MB` (8) backed, the rest unbacked (a fault there is an overflow) |
| The game's thread | `host/hos_main.c:1430`: 64 MiB | the same |
| Default `CreateThread` | `win32_posix.c:257`: 8 MiB | the same, 8 MiB in direct memory |
| Fibers | `hos_thread.c:1002`: `CreateFiberEx(64 KiB, 64 MiB)` | `win32_posix.c:610`: 64 MiB backed whole (phase 4: top only) |
| `std::thread` in gpu/vk | `gpu/vk/vk_jobs.cpp:59`, `vk_shader.cpp:2033`, `vk_replay.cpp`... | the platform's wrap: 2 MiB in direct memory |
| MXCSR | the a64 FPX1 fast paths need round-to-nearest, DAZ and FTZ clear (`gen/a64_suyu.h:391`) | `ps5_fp_ieee()` at `main` (`ps5_platform.c` `hos_ps5_start`) and on every thread `win32_posix.c` starts (`:252`); the wrap gives other threads their creator's MXCSR |
| TLS in hot paths | `hos_thread.c:65` (`cur`: `hos_self`, `hos_ctx`), `nn_os_sync.c:198`, `nvdrv.c:250`, `nvn_native.c:74`, `win32_posix.c:40/208/590` | emulated: ~3.4 ns a lookup (PROBE.md). None in lifted code |
| Affinity | `hos_thread.c:544` (`hos_reserved_cpus`), `hos_thread.c:88`, `HOS_PIN` | `SetThreadAffinityMask` on `ps5_pthread_setaffinity_np` (`win32_posix.c:334`); 13 CPUs, mask 0x1fff |
| Topology | `GetLogicalProcessorInformation` (`win32_posix.c:668`) | pairs (2k, 2k+1) assumed: verify with the platform's topology probe |
| Priorities | `SetThreadPriority` (`win32_posix.c:317`) | recorded only (FreeBSD's `setpriority` would take the process); phase 6: `scePthreadSetprio` |
| Futexes | condition variables, `WaitOnAddress` (`win32_posix.c:97`) | `_umtx_op(UMTX_OP_WAIT_UINT_PRIVATE / WAKE_PRIVATE)` (`ps5_platform.c:144`) |
| Thread ids, names, exit | | `pthread_getthreadid_np`, `pthread_set_name_np` (`pthread_setname_np` is NULL in a title), `ps5_pthread_exit` (its `thread_local` destructors first) |

**Pipeline compiles need more than 1.5 MiB of stack** (PLAN.md, r8: RADV segfaulted at 1 MiB, passed at 1.5 MiB).
The 2 MiB a `std::thread` gets leaves little margin. The rule for the GPU side: every thread that calls
`vkCreate*Pipelines` runs on a stack of 8 MiB or more, made by `CreateThread(NULL, 8 << 20, ...)` (whose stack this
branch places in direct memory) rather than `std::thread`. Asking `pthread_attr_setstacksize` for 8 MiB is not enough
on the console: the wrap then lets libkernel take it from flexible memory.

Phase 4: `_umtx_op` first (every lock and condition variable rests on it; fallback: a condition variable per hashed
address); MXCSR read back on a guest thread, a std::thread and a fiber; then the stacks: a reaper for the stacks of
threads that end (the wrap has one), and growth on touch from an alternate signal stack (`sigaltstack`), so that a
guest thread backs only what it uses.

## 5. Files

| What | Where | On the console |
|---|---|---|
| The game folder | `host/hos_paths.h:62` (`hos_game_path_a`), `hos_main.c:1258` (image.bin, romfs, user beside the exe) | `GetModuleFileNameA` answers `<root>/eboot.bin` (`win32_posix.c`, `ps5_platform.c:180`) |
| romfs | `host/nn_fs.c:14` (`hos_romfs_dir`), `--romfs`; `--nup` and `host/nup_fs.c` (`hos_romfs_nup`) | `<root>/game.nup` read in place (one file, 25.6 GB; one descriptor, a 10.5 MiB index built at stage 4, after the settings), or the file `HOOKSHOT_PS5_NUP` names in `hookshot_env.txt` (lot 88: the extended storage, `/mnt/ext1/grapnel/game.nup`; `host/posix/ps5_game_file.h`), or `<root>/romfs` (186,583 files) when that folder is there |
| Saves, cache, temp | `nn_fs.c:122-172` under `hos_user_dir`; `hos_main.c:311` `saves_dir` (XDG on Linux) | `<root>/user` (no `HOME`: `saves_dir` keeps it beside) |
| Options, env, logs | `hookshot_options.ini`, `hookshot_env.txt` (`hos_main.c:366`), `hookshot_log.txt` (`hos_main.c:1224`) | in `<root>` |
| Mods | `host/mods.c` | `<root>/mods` |
| Renderer caches | `gpu/vk/vk_draw.cpp:639` (`mkw_vk_pipelines.bin`), `hookshot_shaders.bin`, `hookshot_programs.bin` | in `<root>`: made on the PC, sent (below) |
| Working directory | `SetCurrentDirectoryA` (`hos_main.c`), `GetCurrentDirectoryA` | remembered: libc's `getcwd` faults for a title |
| Open files | `nn_fs.c:238`: a host descriptor per guest file | 249 by path at most (PROBE.md): phase 4 routes `CreateFileA`/`ReadFile`/`CloseHandle` (`win32_posix.c:1089`) through `ps5platform/fdv.h` |
| Permissions | `CreateDirectoryA` 0755, `CreateFileA` 0644 (`win32_posix.c:1207`, `:1101`) | phase 4: 0777 and 0666 with `chmod`, for FTP |
| Directory listing | `FindFirstFileA` | the platform's `opendir`/`readdir` (bound by the recipe), our own case-folding matcher (`fnmatch` is NULL in a title) |

**The root.** `/data/hookshot` is PLAN.md's choice, but a title sees only `/app0` and `/download0` until it is
elevated (`ps5_elevation_request`, which needs the Lapy daemon running: "experimental" on firmware 12.60,
PS5_PayloadSDK `platform/docs/ELEVATION.md`; `downloadDataSize` > 0 in `param.json`). So: `/data/hookshot` when
elevation answers OK, `/app0` (`/data/homebrew/<TITLE_ID>` over FTP) otherwise, `HOOKSHOT_PS5_ROOT` over both.
Phase 4 calls `ps5_elevation_request` first in `main`, single-threaded, before klog's thread.

**`/mnt` is hidden** by the sandbox: no USB drive and no extended storage for the romfs; it lives on the internal SSD.

**The write throttle** (about 1.3 GiB at full speed, then about 2 MiB/s, for the process): the renderer's caches
are the problem, not the saves. On the PC build they are `hookshot_programs.bin` 6.9 GB, `mkw_vk_pipelines.bin`
550 MB and `hookshot_shaders.bin` 539 MB. Made on the console, the first two alone would take hours. So they are made
on the PC (the shaders are SPIR-V; the driver's cache with PS5_Mesa built as the console's, bit-identical on the
69 binaries checked in phase 2) and sent with ps5upload, whose writes are not the title's; the title keeps writing
only its saves, its options and a bounded log. A big write that must happen on the console goes through
`ps5platform/offload.h`. Reads are not throttled (1.9-3.7 GiB/s). Phase 4 also checks that the renderer does not
rewrite a cache it read whole (`mkw_vk_pipelines.bin` is read whole at start: 550 MB of memory too).

## 6. Display, controllers, sound

| What | Where | On the console |
|---|---|---|
| Window and events | `host/posix/display_sdl.c:173` (`hos_display_init`), `:210` (`hos_display_loop`) on `hos_sdl.h` | unchanged; `hos_sdl_ps5.c` gives one "window", the TV, and no events (the shell ends the title) |
| The surface | `display_sdl.c:106` (`MkwVkWindow`, `gpu/vk/vk_api.h:164`) -> `hos_sdl_vulkan_surface` | `hos_sdl_ps5.c:60`: `vkGetPhysicalDeviceDisplayPropertiesKHR`, the first mode, `vkCreateDisplayPlaneSurfaceKHR` on RADV's VideoOut WSI; 3840x2160 (or 1920x1080, 2560x1440) |
| Instance extensions | `gpu/vk/vk_main.cpp:101` (xlib, xcb, wayland) | `VK_KHR_display` added to that list (the GPU side's line) |
| Splash | | `sceSystemServiceHideSplashScreen` in `hos_ps5_start` |
| 120 Hz | `host/nn_vi.c:19` (`vsync_main`: the game's 60 Hz pacing) | `param.json` `attribute3` 0x80040; the WSI falls back to 59.94 Hz |
| Controllers | `host/pad_sdl.c:162` (`hos_sdl_pads`), read by `host/nn_hid.c:257` | `hos_sdl_ps5.c`: scePad (`scePadInit`, the initial user, `scePadOpen` 10 tries 100 ms apart, 120-byte samples); `PAD_MAP` (`hos_sdl_ps5.c:30`) gives the console's buttons in SDL's positional order; sticks 0..255 to -32768..32767; samples with 0x80000000 (the shell's) ignored; rumble `scePadSetVibration` |
| XInput, keyboard, mouse | `nn_hid.c:526` (`LoadLibraryA("xinput1_4.dll")`), `GetAsyncKeyState`, `mouse_native.c` | none: `LoadLibraryA` answers NULL, the keys table is empty (USB keyboard and mouse: later, gap 12) |
| Sound | `host/nn_audio.c:623` `waveOutOpen` (48 kHz S16 stereo, `nn_audio.c:647`) -> `win32_posix.c:1563` -> `hos_sdl_audio_open` | `hos_sdl_ps5.c:79`: `sceAudioOutInit`, `sceAudioOutOpen(0xff, 0, 0, 256, 48000, S16 stereo)`, a worker thread that pulls 256 frames and blocks in `sceAudioOutOutput` |
| Movies | `host/nn_video.c:22` (FFmpeg: VP8, VP9, AV1) | `host/posix/ps5_noffmpeg/` headers and `ps5_stubs.c`: no decoder, the movies skipped. Phase 5: FFmpeg + dav1d cross-built (`--target-os=freebsd --arch=x86_64 --cpu=znver2`), `HOOKSHOT_PS5_FFMPEG=<prefix>` |

Phase 3 leaves `hos_sdl_load` answering 0: the console build then runs as a headless Linux build does (the renderer
offscreen, no pad, no sound), which is phase 4's first runnable state. Each part above then becomes the console's
calls as PS5_VulkanTemplate's `ps5/src/platform.c` makes them (proven on the console in phase 1).

## 7. The process: exit, crash, logs, Vulkan

- **Exit.** `ExitProcess` (`win32_posix.c:473`) is the shell's exit (`hos_ps5_exit`: `fflush`,
  `sceSystemServiceLoadExec("exit", NULL)`, wait), never `exit()`; `catchReturnFromMain` (`ps5_platform.c:237`)
  does the same when `main` returns. The game thread ends the process through `ExitProcess` (`hos_main.c`).
- **Crash handler.** `hos_commit_init` (`mkw_glue.c:214`) installs the commit-on-touch handler, the fatal-error
  handler and `report_crash` (`mkw_glue.c:87`) through `win32_posix.c`'s `sigaction` (`:1384`, `:1421`), which reads
  FreeBSD's `mcontext_t` at the console's place (`win32_posix.c:1348`; `ps5platform/context.h` refuses an SDK whose
  `ucontext_t` lacks the 48 bytes) and chains a fault it does not own to the previous handler, so the console's own
  crash report still names it. `backtrace` and `dladdr` are the platform's (`mkw_glue.c:127`). Phase 4: a guard page
  hit on purpose, the report read in klog, offsets read against the eboot's link map.
- **Logs.** `hos_ps5_start` sends standard error to klog (`ps5_klog_capture_stderr`). But `game_main` reopens
  standard error on `hookshot_log.txt` when it is not a terminal or a file (`hos_main.c:1224`), which ends the
  capture. Phase 4: `hos_log` (`hos_thread.c:240`) writes the line to klog (`sceKernelDebugOutText`) as well, and the
  log file is flushed on a timer.
- **Vulkan.** `vk_main.cpp:33` loads `vulkan-1.dll` and asks `vkGetInstanceProcAddr`; `rx_backend.c:231` loads
  `hookshot_vk.dll` and asks `mkwvk_init`. Both are the module table (`ps5_platform.c:157`): RADV's
  `vk_icdGetInstanceProcAddr` and the renderer's `mkwvk_init`, linked into eboot.bin. They are weak references (host/
  links without the renderer), and a weak reference pulls no archive member in: the link names them with
  `-u mkwvk_init -u vk_icdGetInstanceProcAddr`.
- **Updates and the launcher**: `host/hos_update.c` already has an empty `hos_update_check` for a system that is
  neither Windows nor Linux; nothing to do.

### The eboot's link (`nx2/toolchain_ps5.py` `link_title`, branch `ps5-port`)

`nx2/build.py` links after the archives: `--standalone` (all of the game) makes the game's title, `--runtime` the boot
test's (below), both **PPSA99145 "Hookshot PS5"** in `<work>/sa/build-ps5/ps5/title-{game,boot}/` (`HOOKSHOT_PS5_TITLE_DIR`:
elsewhere; `HOOKSHOT_PS5_NO_LINK=1`: archives only). The steps, after PS5_VulkanTemplate's `ps5/tools/link-title.sh` and
the GPU smoke title's `scripts/ps5/build-smoke.sh` (run on the console):

1. the CRT (PS5_Vulkan `tooling/native/app_crt.cpp`, through `tooling/prospero-clang18`) and the AGC import stubs;
2. `tools/radv-link.sh`'s recipe, sourced in bash and its three arrays read back: RADV whole, libc++/libc++abi/libunwind,
   clang's builtins (`PS5_CLANG`, default the `clang` on PATH), `libps5platform.a`; the heap, thread and stdio wraps;
   the libc names the console lacks bound to `ps5_*`;
3. `prospero-lld -T ps5-pie-unwind.ld --eh-frame-hdr`, the recipe's flags, `--defsym=<name>=0` for every weak reference
   nothing linked defines (the 7 `_ZTH*` of constant-initialized thread_locals, and `hos_ps5_boot` in the game: lld 19
   would make each an import the native tool refuses), `-u mkwvk_init -u
   vk_icdGetInstanceProcAddr`, `--version-script app-symbols.map --exclude-libs=ALL -e _start -Map`, then the CRT,
   `--whole-archive libhookshot_game.a` (or `libhookshot_boot.a`), `libhookshot_host.a`, `libhookshot_rt.a`,
   `--whole-archive libhookshot_vk.a` (gpu/vk, suyu's decoders, `gdi_posix.c` once), `libhookshot_eden.a`,
   `libhookshot_sirit.a`, `libopus.a`, the AGC stubs, the recipe's inputs, `--as-needed` the SDK's `*.so`. No
   `--gc-sections` yet (the proven links have none). `win32_posix.c` is in `libhookshot_host.a` only: one copy;
4. the checks: `localeconv` is the platform's; `surface_elf` over the linked ELF: every dynamic import by the module that
   exports it, and none "null" (only `libkernel_sys` or `libScePosixForWebKit`) or "missing"
   (`python -I nx2/toolchain_ps5.py surface-elf <link-*/llvm-pie.elf>` by hand);
5. `ps5-native-tool link`, `self --sign` to `eboot.bin`, `self --inspect`;
6. the title folder: `eboot.bin`, `sce_sys/param.json` (`param_json()`: the smoke title's, `downloadDataSize` 256,
   `attribute3` 0x80040), `sce_module/libc.prx` (PS5_Vulkan `runtime/`, its sha256 checked).

RADV: `RADV_ARCHIVE` when set; for the game, the driver agent's game RADV when it is built
(`mesa-work/game-build/radv-release-cond2`: the task-shader patch, the mesh count fix and the flat COND_EXEC); otherwise,
and for the boot test, PS5_Vulkan's release archive. The renderer's objects are build.py's own (`RX_CPM` with the
Vulkan, SPIR-V and VMA headers, `scripts/ps5/fetch-headers.sh`), with `-fexceptions -fcxx-exceptions -frtti` on the
console.

**First links (2026-11-09, `ps5-port` at `3df08de`):** the boot test 28,484,909 bytes (stock RADV) and 28,496,653 (the
game RADV `8a8d4832693`), 343 imports; the game 489,648,781 bytes (its ELF 559.6 MB: `.text` 446.8 MB, `.bss` 173 MB in
memory), 345 imports, all answered by modules a title loads. The 80 lifted files compiled in 3,051 s at 3 jobs (nice
19, beside other builds); the game's link takes 2.4 s on 3 threads. The lifted calls came out direct (`addr32 call`:
`-fno-plt`'s GOT calls relaxed). The PC builder's lift (v2.0.7) stubs one import this host/ implements
(`nn::audio::SetVoiceBiquadFilterParameter<float>`): `build.py` `ps5_stubs` compiles a copy of `a64_stubs.c` without it.
The console's side, file by file and stage by stage: `hookshot-ps5/dist/README-console.md`.

### The stages a console run logs, and the boot test

`main` (`host/hos_main.c`) says one line per stage, `ps5: stage <n> <what>: ...`, in klog (prefix `[hookshot]`), for the
game and the boot test alike: 1 klog, 2 the guest window (how `ps5_window.c` made it: `hos_ps5_guest_stats`), 3 the data
root (`hos_ps5_data_status`), 4 `hookshot_env.txt`, 5 the storage broker (`hb_start("/app0/hookshot-broker.elf")`, the
platform layer's, when `HOOKSHOT_PS5_BROKER=1`: nothing writes through it yet, and it outlives the title; `link_title`
builds `hookshot-broker.elf` into the title folder), then 6 "game: started". The game then keeps klog: on the console
`game_main` no longer reopens standard error on `hookshot_log.txt` (which ended klog's capture); `hos_log` writes each
line to the file as well (`hos_log_tee`, `host/hos_thread.c`), `hos_fatal` too.

The boot test (`scripts/ps5/boot`, `--runtime`): `hos_ps5_boot` (a weak reference in `main`) runs in place of the game:
6 the files (its log, `<root>/hookshot_boot.txt`, and what the root holds), 7 a thread as the game's (64 MiB, MXCSR),
8 memory (`ps5_memory_query`, the host's handlers, a 1 MiB commit/write/read/decommit in the guest heap), 9 the renderer
through the host's own path (`hos_display_init`, `hos_display_start` -> `rx_start` -> the module table -> `mkwvk_init` ->
`start`), 10 frames (`rx_nvn_present` with no picture: a black frame each, through the replay's present and paced by
the display), 11 commit on first touch (`HOOKSHOT_BOOT_TOUCH=1`), 12 the shell's exit. `boot_nogame.c` stands in for the
lift's tables. The console's side of each line: `hookshot-ps5/dist/README-console.md`.

**The display.** With no SDL window (`hos_sdl_load` answers 0 on the console), `host/posix/display_sdl.c`
`hos_display_start` gives the renderer an `MkwVkDisplay` (`gpu/vk/vk_api.h`), so it makes its own `VK_KHR_display`
surface over VideoOut (`gpu/vk/vk_ps5.cpp`): `HOOKSHOT_PS5_DISPLAY=<w>x<h>[@120]`. The TV is then the window in front
(`hos_posix_window[0] = 1`): `nn_hid.c`'s `game_in_front` and the Insert menu need it.

## 8. Phase 4, in PLAN.md's order

| Step | Code | Exit check on the console |
|---|---|---|
| 1. klog, crash handler | `hos_ps5_start`, `hos_log` to klog, `mkw_glue.c` handlers | a deliberate fault reported in klog with the eboot's offsets; the console's report still written |
| 2. Memory | `ps5_platform.c` window functions, `mkw_glue.c:18`, `:63` | the 512 GiB reservation; 4 GiB touched at random (backed by faults), decommitted, touched again as zero; one RADV import of committed units |
| 3. Threads, MXCSR | `win32_posix.c` futexes, `CreateThread`, `hos_ps5_stack` | `_umtx_op` wait/wake/timeout; MXCSR 0x1f80 on each kind of thread; 40 guest-like threads with 64 MiB stacks |
| 4. Files | the root, `fdv.h`, modes, elevation | romfs read at full speed; 1,000 files open; a save written and read over FTP |
| 5. Display | `hos_sdl_vulkan_surface`, `VK_KHR_display` | a cleared frame presented at 4K, 120 Hz requested |
| 6. Pad | `hos_sdl_pad_*` | the Insert menu driven from the DualSense |
| 7. Sound | `hos_sdl_audio_open` | a tone through `waveOut` |
| 8. Shell exit | `hos_ps5_exit` | the home screen back, no crash report |

The exit test of the phase: a test title with the runtime and a small guest program, and `tests/` that need no game.

## 9. Census

Every gap met in phase 3, by area. State: **done** (written and compiled, not run), **stub** (compiles, does
nothing yet), **open** (phase 4 unless said), **risk** (may block), **n/a** (nothing to do, and why).

Counts (`ps5-plat`, 2026-11-09, after the probe's four console runs): 86 entries: 59 done, 2 stub, 16 open,
2 risk, 8 n/a (phase 3: 84 entries, 41 done, 5 stub, 25 open, 5 risk, 8 n/a). "Done" is compiled and, where
the row says "measured", run on the console by PPSA99147. By area: build 10 (8 done, 2 open); memory 18 (12 done, 3 open, 1 risk, 2 n/a); threads
13 (7 done, 5 open, 1 risk); files 13 (11 done, 1 open, 1 n/a); display 8 (4 done, 1 stub, 2 open, 1 n/a); input 5 (3 done, 1 open, 1 n/a); sound and video
4 (2 done, 1 stub, 1 open); process 7 (4 done, 3 open); runtime surface and tooling
8 (4 done, 1 open, 3 n/a). The two risks left: the memory budget (M12: 9,216 MiB of guest memory held beside
RADV's device, the renderer's own needs not yet measured) and the pipeline compiles' stacks (T5, GPU side).

| # | Area | Gap | Where | State | Answer on the console |
|---|---|---|---|---|---|
| B1 | build | No PS5 target in the build | `nx2/build.py` | done | `RX_TARGET=ps5`, `nx2/toolchain_ps5.py` |
| B2 | build | `-march` from the build PC's CPU | `nx2/build.py` (`host_march`) | done | `-march=x86-64-v3 -mtune=znver2` forced; `RX_MARCH` ignored |
| B3 | build | One executable and a `.so` | `nx2/build.py` link | done | six static archives in `build-ps5/ps5/` |
| B4 | build | The eboot's link | `toolchain_ps5.link_title` | done | link-title.sh's steps with RADV's recipe; `-u` for the weak entry points, `--defsym=0` for the weak references nothing defines |
| B5 | build | Warnings hidden (`-Wno-everything`) hid implicit declarations | `nx2/build.py` | done | `-Werror=implicit-function-declaration -Werror=int-conversion` on host/ |
| B6 | build | Both targets in one object folder | `nx2/build.py` | done | `build-ps5/` |
| B7 | build | `-fno-plt` calls through the GOT in lifted code | `code_*.o` | done | relaxed to direct calls by `prospero-lld -pie` (checked on a test; phase 5 on the eboot) |
| B8 | build | Lifted chunks compile for the console | `code_012.c` | done | 72 s, 1.5 GB, 0 errors (Linux 76 s) |
| B9 | build | gpu/vk needs Vulkan/SPIR-V headers from a CPM cache (none on this PC) | `nx2/build.py` `cpm_package` | done | `RX_CPM=third_party-ps5/cpm` (`scripts/ps5/fetch-headers.sh`) |
| B10 | build | zbic is a PC tool | `nx2/zbic.py` | done | built for the console too, linked into nothing |
| M1 | memory | Window at 16 TiB; nothing granted at or above 1 TiB | `a64.h:61` | done | `A64_HOST_BASE=0x8000000000` on every compile |
| M2 | memory | One 512 GiB reservation at 0x80_0000_0000 refused on the console (boot test PPSA99145) | `ps5_window.c` `hos_ps5_guest_reserve` | done | measured: the title's map ends at 0xFC_0000_0000; 496 GiB from the base granted as 31 pieces of 16 GiB, `hos_space_end` 496 GiB (plan C'); A64_HOST_BASE stays |
| M3 | memory | `MAP_NORESERVE`, `MAP_FIXED_NOREPLACE`, memfd: none | `hos_main.c:144` | done | `ps5_vrange_reserve_at` |
| M4 | memory | Commit is physical on the console | `mkw_glue.c:18` | done | eager up to 256 MiB, first touch above |
| M5 | memory | Commit on first touch from a signal handler | `mkw_glue.c` commit_on_touch, `ps5_window.c` | done | measured: 8 threads faulting at once, no deadlock, 139 us a fault (1 MiB) |
| M6 | memory | The kernel's own writes into unbacked guest memory (EFAULT) | `win32_posix.c:1120`, `:1136` | done | `ReadFile`/`WriteFile` back their buffers; the image eager |
| M7 | memory | `MADV_REMOVE`/`MADV_DONTNEED` | `hos_main.c:224` | done | `ps5_vrange_decommit` |
| M8 | memory | `/proc/self/maps` for `VirtualQuery` | `win32_posix.c:807` | done | the unit bitmap; `sceKernelQueryMemoryProtection` elsewhere |
| M9 | memory | Write watch | `nx2/rtgen.py:32` | n/a | the soft write watch, as on Linux |
| M10 | memory | `a64_dirty`: 128 MiB of `.bss` | `nx2/rtgen.py:32` | open | measured (probe run 1): charged at load, 285 MiB of flexible free of 448; not touching it changes nothing. Move it to direct memory if flexible memory runs short |
| M11 | memory | 16 KiB pages, 64 KiB units | `hos_mem.c`, `ps5_platform.c` | done | the platform rounds; partial decommits zeroed |
| M12 | memory | The budget: 9809 MB told, ~12 GiB for everything | `hos_mem.c:51` | risk | `HOS_MEMORY_MB`, Minimum preset, `ps5_memory_query` |
| M13 | memory | libc's heap is flexible memory | the link | open (phase 5) | the recipe's `--wrap=malloc` |
| M14 | memory | udmabuf import (Linux) | `vk_memory.cpp:358` | n/a | `__linux__` only; the host-pointer path |
| M15 | memory | Host-pointer import of unit-mapped memory | `vk_memory.cpp:353`, `ps5_window.c` | done | measured: 64 MiB of 1,024 units imported and written by the GPU, a commit across it while imported, a first-touch-backed range: every word right |
| M16 | memory | Unplaced reservations could take the window's place | `hos_main.c:1423` | done | the window reserved first in `main` |
| M17 | memory | Renderer caches read whole (`mkw_vk_pipelines.bin` 550 MB) | `vk_draw.cpp:639` | open (GPU side) | memory and load time: map or stream |
| M18 | memory | The guest's address space when the window is shorter | `hos.h` HOS_SPACE_END, `ps5_window.c` | done | `hos_space_end` (regions from its end); A64_HOST_BASE 0x7C_0000_0000 if the map ends at 0xFC_0000_0000 |
| T1 | threads | Stacks over 2 MiB come from flexible memory | `win32_posix.c:257` | done | placed by us: `hos_ps5_stack` |
| T2 | threads | 64 MiB a guest thread, physical | `hos_thread.c:471`, `hos_main.c:1430` | done | 64 MiB reserved, 8 MiB backed (`HOOKSHOT_PS5_STACK_MB`) |
| T3 | threads | Stacks of ended threads not freed | `win32_posix.c:257` | open | a reaper |
| T4 | threads | Fibers: 64 MiB backed whole | `win32_posix.c:610` | open | top only, grow on touch (`sigaltstack`) |
| T5 | threads | Pipeline compiles need > 1.5 MiB of stack | `vk_jobs.cpp:59`, `vk_shader.cpp:2033` | risk (GPU side) | compile workers on `CreateThread(8 MiB)` |
| T6 | threads | MXCSR 0x9fe0 at start; FPX1 needs IEEE | `gen/a64_suyu.h:391` | done | `ps5_fp_ieee` at `main` and per thread |
| T7 | threads | `SYS_futex` | `win32_posix.c:97`, `ps5_platform.c` | done | `_umtx_op`, condition variables by address if refused (the probe runs both) |
| T8 | threads | `gettid`, `pthread_setname_np` (NULL), `pthread_exit` order | `win32_posix.c` | done | `pthread_getthreadid_np`, `pthread_set_name_np`, `ps5_pthread_exit` |
| T9 | threads | `cpu_set_t`, `sched_getaffinity` | `win32_posix.c:334` | done | `cpuset_t` through `ps5_pthread_[gs]etaffinity_np`; 0x1fff |
| T10 | threads | Topology from `/sys` | `win32_posix.c:668` | open | pairs assumed; the platform's topology probe |
| T11 | threads | Priorities | `win32_posix.c:317` | open (phase 6) | `scePthreadSetprio` |
| T12 | threads | Emulated TLS in host hot paths | `hos_thread.c:65`, `nn_os_sync.c:198`, `nvdrv.c:250` | open (phase 6) | measure; keep in `HosThread` |
| T13 | threads | Fibers on `getcontext`/`makecontext`/`swapcontext` | `win32_posix.c`, `ps5_fiber.c` | done | the ucontext switch corrupted the heap on the console (probe run 1): an own switch (callee-saved registers, MXCSR, x87 control word), 3,000 switches on the host model |
| F1 | files | No `/data` without elevation | `ps5_platform.c` `hos_ps5_data_init` | done | `/data/hookshot` when the sandbox has `/data` (ShadowMountPlus 1.7beta3+), Lapy on request (`/app0/hookshot-elevate`), else `/app0`; the storage broker writes there in any case |
| F2 | files | Paths beside the executable | `hos_paths.h:62`, `hos_main.c:1258` | done | `GetModuleFileNameA` = `<root>/eboot.bin` |
| F3 | files | `$HOME`/XDG saves | `hos_main.c:311` | done | no `HOME`: `<root>/user` |
| F4 | files | `getcwd` faults, `chdir` untrusted | `win32_posix.c` | done | remembered |
| F5 | files | `/proc/self/exe` | `win32_posix.c` | done | the root's path |
| F6 | files | `fnmatch` NULL in a title | `win32_posix.c:1255` | done | own matcher (checked against glibc) |
| F7 | files | `/mnt` hidden | | n/a | romfs on the internal SSD |
| F8 | files | Write throttle (caches 7.9 GB) | `ps5_files.c`, `ps5_stdio.c`, `ps5_broker_*` | done | measured: the title's own writes 243 MiB/s for ~1 GiB, then 16 MiB/s; the broker 151 MiB/s for 2 GiB. WriteFile (ps5_files.c) and fopen/fwrite (ps5_stdio.c, the eboot's --wrap) under /data/hookshot go through it from 16 KiB |
| F9 | files | 249 files open by path | `nn_fs.c`, `nup_fs.c`, `ps5_files.c` | done | measured: dup2 over a file is refused to a title (EPERM), so fdv cannot park files and is off (HOOKSHOT_PS5_FDV=1 tries it); the RomFS is one descriptor (game.nup); an EMFILE is said once. If a game ever meets the limit: a cache of open files in win32_posix.c's handles |
| F10 | files | Modes for FTP and the broker | `ps5_files.c`, `ps5_stdio.c` | done | files under /data made 0666 (folders 0777), and opened up when written; the broker is another identity |
| F11 | files | `access` refused | `hos_paths.h:77` | done | the recipe's `ps5_access` |
| F12 | files | stdio writes slow (13 MiB/s) | logs | open | logs bounded, flushed on a timer |
| F13 | files | A big write must not be the title's | `ps5_broker_*` | done (probe) | the storage broker on 127.0.0.1:9147, started through the ELF loader |
| F14 | files | The romfs as 186,583 files: made at ~2 MB/s on the console, a descriptor per open file | `nn_fs.c`, `hos_main.c` | done (PC; the probe's nup check on the console) | `host/nup_fs.c`: the RomFS read in place from the user's `.nup` (game.nup in the root when there is no romfs/; `--nup`): the newest title's romfs indexed at start (186,583 files, 599 folders, 10.5 MiB, 22 ms on the PC), every read a pread on one descriptor; mods over it; `scripts/ps5/nup/check.sh` compares it with pc-linux/romfs, file by file and through nn_fs.c's own calls |
| D1 | display | No window system | `hos_sdl.c` | stub | `hos_sdl_ps5.c` on `VK_KHR_display` |
| D2 | display | `VK_KHR_display` not asked | `vk_main.cpp:101` | open (GPU side) | one name in the list |
| D3 | display | Vulkan loader | `vk_main.cpp:33` | done | `vk_icdGetInstanceProcAddr` through the module table |
| D4 | display | `hookshot_vk.so` | `rx_backend.c:231` | done | `mkwvk_init` through the module table |
| D5 | display | `dlsym` for the window's state | `win32_posix.c:1476` | done | weak references |
| D6 | display | Splash | `ps5_platform.c` | done | `sceSystemServiceHideSplashScreen` |
| D7 | display | 120 Hz | `param.json` | open (phase 5) | `attribute3` 0x80040 |
| D8 | display | Message boxes | `win32_posix.c:1522` | n/a | stderr (klog) |
| P1 | input | SDL controllers | `pad_sdl.c:162`, `hos_sdl_ps5.c` | done | measured: scePad behind hos_sdl.h, buttons by position, sticks, triggers; handles kept while the user is signed in, presence with a 2 s grace (no flapping when unattended) |
| P2 | input | Rumble | `hos_sdl_ps5.c` | done | `scePadSetVibration`, compatible mode |
| P3 | input | Several users | `hos_sdl_ps5.c` | done | a player per signed-in user, read again once a second |
| P4 | input | XInput | `nn_hid.c:526` | n/a | `LoadLibraryA` NULL |
| P5 | input | Keyboard, mouse | `hos_sdl_keys` | open (later) | USB, gap 12 |
| S1 | sound | SDL audio | `win32_posix.c:1563`, `hos_sdl_ps5.c` | done | sceAudioOut 48 kHz S16 stereo, a thread pulling waveOut's queue |
| S2 | sound | Opus | `third_party/opus` | done | builds |
| S3 | video | FFmpeg | `nn_video.c:22` | stub | shim; FFmpeg + dav1d cross-built in phase 5 |
| S4 | video | Media Foundation AV1 | `linux_stubs.c` | open (phase 5) | dav1d |
| X1 | process | `exit()` | `win32_posix.c:473` | done | the shell's exit; `catchReturnFromMain` |
| X2 | process | Signals: Linux `gregs` | `win32_posix.c:1348` | done | FreeBSD `mcontext_t`, `context.h` |
| X3 | process | Crash report through klog | `mkw_glue.c:87` | open | klog lines, the eboot's map |
| X4 | process | `freopen(stderr)` ends klog's capture | `hos_main.c` `game_main` | done | no freopen on the console: `hos_log` copies each line to `hookshot_log.txt` |
| X5 | process | Linux profilers (`perf_event_open`) | `host/posix/prof.c` | done | left out; `hos_prof_start` says so |
| X6 | process | Launch arguments (`argv[0]` "") | `hos_main.c` | done | the loop from 1 skips it |
| X7 | process | Stack overflow faults need a stack to run on | `win32_posix.c:1421` | open | `sigaltstack` per thread |
| R1 | surface | `timegm` exported by nothing | `windows.h` `_mkgmtime` | done | `hos_ps5_timegm` (checked against glibc, 200,000 dates) |
| R2 | surface | `isatty` NULL in a title | `win32_posix.c` | done | `ps5_isatty` |
| R3 | surface | 129 imports a module exports, never called by a title yet | the archives | open | proven by the runs; first `_umtx_op`, `pthread_attr_setstack`, `sceKernelQueryMemoryProtection`, the ucontext calls |
| R4 | surface | 21 imports the platform answers | the recipe | done | `access`, `backtrace`, `dladdr`, the directory family, `clock_nanosleep`, `gmtime_r`, `localtime_r`... |
| R5 | surface | `sysconf` is the console's (not bound) | `win32_posix.c` | n/a | only `_SC_PAGESIZE` (16384) is asked |
| R6 | surface | `__emutls_get_address` | the recipe | done | clang's builtins |
| R7 | tooling | Debug registers, `SuspendThread` | `mkw_glue.c` watch | n/a | unsupported on POSIX already |
| R8 | tooling | Page watches (debugging) at 4 KiB | `nvn_native.c:783` | n/a | rounded to 16 KiB |

The surface counts come from `python -I nx2/toolchain_ps5.py surface` over the four runtime archives, with RADV's
archive as the other side: 129 module, 21 platform, 1 runtime, 1 ours (RADV's ICD entry), 1 the console's own
(`sysconf`), 20 "missing" that the game's tables (`a64_fn_*`, `hos_entry_*`, `hos_image_*`, from the lift) and the
renderer (`mkwvk_init`) define. No import is NULL at run time any more.

## 10. Coordination with the GPU side (`ps5-gpu`)

This branch changes nothing under `gpu/`. What `gpu/vk` needs, for the GPU side to make:

1. `gpu/vk/vk_main.cpp:101`: `"VK_KHR_display"` in the list of surface extensions (harmless on Linux: the list is
   filtered by what the loader has).
2. The threads that build pipelines on `CreateThread(NULL, 8 << 20, ...)`, not `std::thread` (section 4).
3. `RX_CPM` with Vulkan-Headers, SPIRV-Headers and VMA, for `nx2/build.py` to add the renderer's jobs. With
   `RX_TARGET=ps5` they get `prospero-clang++`, the console's CPU flags and `host/posix` on the include path, no
   `-fPIC`, and `gdi_posix.c` without a second `win32_posix.c` (one eboot shares the runtime's); they land in
   `libhookshot_vk.a`.
4. `__linux__` stays the switch for udmabuf and dma-buf (`vk_memory.cpp:358`), which the console has not.
5. The caches (`hookshot_programs.bin`, `mkw_vk_pipelines.bin`, `hookshot_shaders.bin`) are made on the PC and only
   read on the console (section 5).
