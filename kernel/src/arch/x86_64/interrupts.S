/* Interrupt entry stubs: 256 of them, each exactly 16 bytes apart, so the
   stub for vector N is at interruptStubs + N * 16 (see idt.cpp).

   Each stub makes the stack look the same (error code, then vector) and
   jumps to interruptCommon, which saves the registers into an Arch::Frame
   and calls interruptDispatch(Frame*). */

    .section .text

    .global interruptStubs
    .balign 16
interruptStubs:
    .set vector, 0
    .rept 256
        .balign 16
        /* These exceptions push an error code themselves. */
        .if !((vector == 8) || ((vector >= 10) && (vector <= 14)) || (vector == 17) || (vector == 21) || (vector == 29) || (vector == 30))
            pushq $0
        .endif
        pushq $vector
        jmp interruptCommon
        .set vector, vector + 1
    .endr

interruptCommon:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    call interruptDispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp /* drop vector and error code */
    iretq

    .section .note.GNU-stack, "", @progbits
