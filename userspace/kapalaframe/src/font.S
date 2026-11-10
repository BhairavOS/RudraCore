/* The Kapalaframe system font: the same 8x16 PSF font as the kernel's
   console (Terminus), copied to build/font.psf by the Makefile. */

    .section .rodata
    .balign 16
    .global _kapalaFontStart
_kapalaFontStart:
    .incbin "build/font.psf"
    .global _kapalaFontEnd
_kapalaFontEnd:

    .section .note.GNU-stack, "", @progbits
