/* (ps5) The boot test's stand-ins for the lift's tables (gen/a64_table.c, gen/hos_image.c): what host/ and a64-kit's runtime
   name of the game, empty. The boot test (hookshot_boot.c) runs none of the game, so nothing here is ever read: it only lets
   host/ link without the lifted code (nx2/build.py --runtime). Contains nothing of the game.

   SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef __PROSPERO__
#include <stdint.h>

typedef void (*BootNoFn)(void);

const uint32_t a64_fn_count = 0;
const uint32_t a64_fn_addr[1] = {0};
const BootNoFn a64_fn_ptr[1] = {0};

const uint64_t hos_main_stack_size = 0x100000ULL;
const int hos_main_priority = 44, hos_main_core = 0;
const uint64_t hos_image_base = 0x80000000ULL, hos_image_end = 0x80000000ULL;   /* (no image: nothing to load) */
const int hos_module_count = 0;
const uint64_t hos_module_init[1] = {0};
const char *const hos_module_name[1] = {"none"};
const uint64_t hos_entry_nninitStartup = 0, hos_entry_nnMain = 0;
const uint64_t hos_program_id = 0;
const char hos_main_build_id[] = "boot-test";
const uint64_t hos_entry_init_libc0 = 0, hos_entry_init_libc1 = 0, hos_entry_init_libc2 = 0, hos_entry_nnosInitialize = 0;
const uint64_t hos_entry_nninitInitializeSdkModule = 0, hos_entry_nvnBootstrapLoaderInternal = 0;
const uint64_t hos_thread_start_hook = 0, hos_thread_exit_hook = 0;
#endif
