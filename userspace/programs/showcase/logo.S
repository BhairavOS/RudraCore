/* The Bhairav logo for the showcase: 256x256 RGBA, converted at build time
   from assets/bhairav-logo.jpg (see the Makefile). */

    .section .rodata
    .balign 16
    .global _showcaseLogo
_showcaseLogo:
    .incbin "build/logo.rgba"

    .section .note.GNU-stack, "", @progbits
