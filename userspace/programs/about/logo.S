/* The Bhairav logo: 256x256 RGBA, converted at build time from
   assets/bhairav-logo.jpg (see the Makefile). */

    .section .rodata
    .balign 16
    .global _aboutLogo
_aboutLogo:
    .incbin "build/logo.rgba"

    .section .note.GNU-stack, "", @progbits
