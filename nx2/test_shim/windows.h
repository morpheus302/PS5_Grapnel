/* (traducteur Triforce, 17/11/2026) What the game-free harnesses of nx2/test_insn.py (a64-kit's insn_test.py) and
   nx2/test_leaf.py take from Windows, on Linux: VirtualAlloc at a fixed address (mmap, MAP_FIXED_NOREPLACE) and __try /
   __except around a call (a fault is caught by a signal handler and siglongjmp; GetExceptionCode says 0xC0000005). Found
   first by those harnesses' -I on Linux only; never part of the game's build. */
#pragma once
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#define EXCEPTION_EXECUTE_HANDLER 1
static inline void *VirtualAlloc(void *addr, size_t size, unsigned type, unsigned protect) {
    (void)type; (void)protect;
    void *r = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    return r == MAP_FAILED ? NULL : r;
}
static sigjmp_buf a64t_jb;
static volatile int a64t_code;
static void a64t_on_fault(int sig) { a64t_code = sig == SIGFPE ? (int)0xC0000094 : sig == SIGILL ? (int)0xC000001D : (int)0xC0000005; siglongjmp(a64t_jb, 1); }
static inline void a64t_install(void) {
    static int done;
    if (done) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = a64t_on_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGFPE, &sa, 0);
    done = 1;
}
#define __try if ((a64t_install(), sigsetjmp(a64t_jb, 1)) == 0)
#define __except(x) else
#define GetExceptionCode() a64t_code
