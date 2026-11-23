/* Hookshot on the PS5: the Win32 calls gpu/vk makes (host/posix/windows.h), for the smoke title only.

   On Linux, hookshot_vk.so carries its own copy of host/posix/win32_posix.c. That file does not build for the console
   (linux/futex.h, SYS_futex, SYS_gettid, sched_getaffinity, /proc/self/maps, /proc/self/exe), and porting it is the host
   side's work (host/, branch ps5-host, which has it): the game's eboot gets these functions once, from there. The smoke
   title links this instead: exactly the 19 non-GDI Win32 functions libhookshot_vk.a imports (llvm-nm -u,
   scripts/ps5/build-gpu.sh), as small as they can be. The GDI ones (the Insert menu's text) are host/posix/gdi_posix.c,
   which builds as it is.

   The game folder for gpu/vk's files (hookshot_options.ini, the program and pipeline caches): GetModuleFileNameA answers
   /app0/eboot.bin, so they are read and written in the title folder.

   SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int sceKernelUsleep(unsigned int microseconds);

void Sleep(DWORD ms) { sceKernelUsleep(ms ? ms * 1000u : 0u); }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }   /* (Win32's pseudo handle) */
BOOL SetThreadPriority(HANDLE t, int p) { (void)t, (void)p; return TRUE; }
DWORD_PTR SetThreadAffinityMask(HANDLE t, DWORD_PTR mask) { (void)t; return mask ? mask : 1; }
HRESULT SetThreadDescription(HANDLE t, PCWSTR name) { (void)t, (void)name; return 0; }

/* Threads with a stack of the size asked for, placed by us in direct memory (the platform heap: the link recipe wraps
   malloc), as ps5-host's CreateThread does (its hos_ps5_stack): a thread that asks the platform's pthread_create for more
   than its 2 MiB gets it from libkernel, out of the title's 448 MiB of flexible memory. gpu/vk's compile threads ask for
   8 MiB (vk_internal.h CompileThread). Only what the smoke title needs: INFINITE waits, and a stack kept until the join
   (a detached thread's is not freed) */
typedef struct {
    pthread_t th;
    void *stack;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
} SmokeThread;
static void *smoke_thread_main(void *p) {
    SmokeThread *t = (SmokeThread *)p;
    return (void *)(uintptr_t)t->fn(t->arg);
}
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id) {
    (void)attr, (void)flags;
    const SIZE_T size = stack ? (stack + 16383) & ~(SIZE_T)16383 : (SIZE_T)8 << 20;
    SmokeThread *t = (SmokeThread *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->stack = aligned_alloc(16384, size);
    t->fn = fn, t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (t->stack) pthread_attr_setstack(&a, t->stack, size);
    const int r = t->stack ? pthread_create(&t->th, &a, smoke_thread_main, t) : -1;
    pthread_attr_destroy(&a);
    if (r) {
        free(t->stack), free(t);
        return NULL;
    }
    if (id) *id = 0;
    return (HANDLE)t;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    (void)ms;   /* (INFINITE only) */
    SmokeThread *t = (SmokeThread *)h;
    if (!t || !t->fn) return 0xFFFFFFFFu;
    pthread_join(t->th, NULL);
    free(t->stack);
    t->stack = NULL, t->fn = NULL;   /* (joined: CloseHandle frees the rest) */
    return 0;   /* (WAIT_OBJECT_0) */
}
BOOL CloseHandle(HANDLE h) {
    SmokeThread *t = (SmokeThread *)h;
    if (!t) return FALSE;
    if (t->fn) { pthread_detach(t->th); return TRUE; }   /* (still running: it keeps its stack and its record) */
    free(t);
    return TRUE;
}

DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap) {
    (void)m;
    const int n = snprintf(out, cap, "%s", "/app0/eboot.bin");
    return n < 0 || (DWORD)n >= cap ? cap : (DWORD)n;
}
DWORD GetFileAttributesA(LPCSTR path) {
    struct stat st;
    if (stat(path, &st)) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL DeleteFileA(LPCSTR path) { return unlink(path) == 0; }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(to, &st) == 0) return FALSE;
    return rename(from, to) == 0;
}

/* (no keyboard, no window: the Insert menu's keys and the window messages do nothing) */
SHORT GetAsyncKeyState(int vk) { (void)vk; return 0; }
HWND GetForegroundWindow(void) { return NULL; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) { (void)w; if (pid) *pid = 0; return 0; }
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { (void)w, (void)msg, (void)wp, (void)lp; return FALSE; }
LONG GetWindowLongA(HWND w, int index) { (void)w, (void)index; return 0; }

/* (memory: the guest window is the host's; the smoke title has no guest. vk_memory.cpp commits reserved guest pages
   before importing them, and vk_bank.cpp asks whether a pointer is committed: answered for ordinary memory) */
void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) {
    (void)protect;
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) return addr;
    void *p = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size) {
    if (size < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = (void *)((uintptr_t)addr & ~(uintptr_t)16383);   /* (the console's 16 KiB pages) */
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 16384;
    mbi->State = MEM_COMMIT, mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE, mbi->Type = MEM_PRIVATE;
    return sizeof *mbi;
}
