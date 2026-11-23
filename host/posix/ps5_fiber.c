/* (ps5) Fibers on the console: host/posix/ps5.h's hos_ps5_fiber_*, for win32_posix.c's CreateFiberEx and SwitchToFiber.
   Built with RX_TARGET=ps5, and on a PC with HOOKSHOT_PS5_HOST_MODEL (scripts/ps5/build-plat-probe.sh host-test switches
   fibers with it). */
#if defined(__PROSPERO__) || defined(HOOKSHOT_PS5_HOST_MODEL)
#include "ps5.h"

#include <stdint.h>
#include <string.h>

/* ---- fibers: a context switch of our own. win32_posix.c's fibers were getcontext/makecontext/swapcontext, never run by a
   title before the platform probe (census T13): its first fiber came back from SwitchToFiber with the heap corrupted
   (DeleteFiber's free crashed, PPSA99147 run 1), the console's context records not being what the SDK's ucontext_t
   holds (they carry 48 bytes more before the registers already, PROBE.md; the kernel's FPU state may be larger still).
   The switch saves what the SysV ABI has a callee keep (rbx, rbp, r12-r15, the stack, MXCSR and the x87 control word)
   on the old stack and loads the new one's; nothing else is written anywhere ---- */
__asm__(".text\n"
        ".globl hos_ps5_fiber_switch\n"
        ".type hos_ps5_fiber_switch,@function\n"
        "hos_ps5_fiber_switch:\n"   /* (void **save_rsp: %rdi, void *to_rsp: %rsi) */
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $8, %rsp\n"
        "    stmxcsr (%rsp)\n"
        "    fnstcw 4(%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    ldmxcsr (%rsp)\n"
        "    fldcw 4(%rsp)\n"
        "    addq $8, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size hos_ps5_fiber_switch, .-hos_ps5_fiber_switch\n"
        ".type hos_ps5_fiber_trampoline,@function\n"
        "hos_ps5_fiber_trampoline:\n"   /* (a new fiber's first switch returns here: entry in r12, its argument in r13) */
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size hos_ps5_fiber_trampoline, .-hos_ps5_fiber_trampoline\n");
extern char hos_ps5_fiber_trampoline[];
void *hos_ps5_fiber_init(void *stack, size_t size, void (*entry)(void *), void *arg) {
    /* (the new stack as hos_ps5_fiber_switch leaves one: from its 16-aligned top S, the trampoline at S-24, so that after
       the ret the stack is 16-aligned for its call; rbp, rbx, r12 (entry), r13 (arg), r14, r15 below it; then MXCSR and the
       x87 control word, the creator's) */
    uint64_t *const top = (uint64_t *)(((uintptr_t)stack + size) & ~(uintptr_t)15);
    uint64_t *const sp = top - 10;
    memset(sp, 0, 10 * sizeof *sp);
    unsigned m = 0;
    unsigned short cw = 0;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(m), "=m"(cw));
    m &= ~0x3fu;   /* (no sticky exception flags carried over) */
    memcpy(sp, &m, 4);
    memcpy((char *)sp + 4, &cw, 2);
    sp[1] = 0, sp[2] = 0;                                    /* r15, r14 */
    sp[3] = (uint64_t)(uintptr_t)arg;                        /* r13 */
    sp[4] = (uint64_t)(uintptr_t)entry;                      /* r12 */
    sp[5] = 0, sp[6] = 0;                                    /* rbx, rbp */
    sp[7] = (uint64_t)(uintptr_t)hos_ps5_fiber_trampoline;   /* the return address, at S-24 */
    return sp;
}
#endif /* __PROSPERO__ || HOOKSHOT_PS5_HOST_MODEL */
