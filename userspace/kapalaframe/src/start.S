/* KapalaFrame program entry point. The kernel starts every program here,
   in user mode, with a fresh stack. */

    .section .text
    .global _start
_start:
    xorl %ebp, %ebp     /* end of the backtrace chain */
    andq $-16, %rsp     /* the ABI wants a 16-byte aligned stack at calls */
    call kapalaStart
    ud2                 /* kapalaStart never returns */

    .section .note.GNU-stack, "", @progbits
