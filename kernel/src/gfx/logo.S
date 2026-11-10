/* Embeds the Bhairav logo: 256x256 RGBA pixels, converted at build time
   from assets/bhairav-logo.jpg (see the Makefile). */

    .section .rodata
    .balign 16
    .global _logoStart
_logoStart:
    .incbin "build/logo.rgba"
    .global _logoEnd
_logoEnd:

    .section .note.GNU-stack, "", @progbits
