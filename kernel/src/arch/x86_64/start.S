/* Kernel entry point. Limine's stack lives in bootloader memory that we want
   to reclaim, so switch to the kernel's own stack before anything else. */

    .section .bss
    .balign 16
kernelStackBottom:
    .skip 64 * 1024
    .global kernelStackTop
kernelStackTop:

    .section .text
    .global _start
_start:
    /* rdi and rsi are left alone: they're kmain's arguments (the boot
       protocol magic and info pointer from ShoolLoader). */
    leaq kernelStackTop(%rip), %rsp
    xorl %ebp, %ebp          /* end of the backtrace chain */
    call kmain
1:
    cli
    hlt
    jmp 1b

    .section .note.GNU-stack, "", @progbits
