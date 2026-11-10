/* Embeds the console font into the kernel's read-only data.
   The Makefile copies the font to build/font.psf before assembling. */

    .section .rodata
    .balign 16
    .global _embeddedFontStart
_embeddedFontStart:
    .incbin "build/font.psf"
    .global _embeddedFontEnd
_embeddedFontEnd:

    .section .note.GNU-stack, "", @progbits
