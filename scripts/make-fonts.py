#!/usr/bin/env python3
"""Renders the Bhairav OS fonts into assets/fonts.bin.

Each face (a font at one pixel size) is rendered once with FreeType (through
Pillow): an anti-aliased coverage bitmap per character, with metrics. Bhairav
OS maps the file into every program, and Kapalaframe draws text from it.

Usage (needs Pillow and the DejaVu fonts):
    python3 make-fonts.py <dejavu-directory> <output-file>

The fonts are DejaVu (Bitstream Vera license; see assets/DEJAVU-LICENSE).

File format, little-endian:
    u32 magic "BFN1", u32 face count
    faces:  char name[16], u16 size, i16 ascent, i16 descent, u16 line height,
            u32 glyph count, u32 offset of its glyphs
    glyphs: u32 codepoint (sorted), i16 left, i16 top (from the baseline, up is
            negative), u16 width, u16 height, u32 advance (1/64 px),
            u32 offset of its coverage bytes (width * height)
"""

import struct
import sys

from PIL import ImageFont

FACES = [
    # name, file, pixel size
    ("sans", "DejaVuSans.ttf", 13),
    ("sans-bold", "DejaVuSans-Bold.ttf", 13),
    ("sans", "DejaVuSans.ttf", 15),
    ("sans-bold", "DejaVuSans-Bold.ttf", 15),
    ("sans-bold", "DejaVuSans-Bold.ttf", 18),
    ("sans-bold", "DejaVuSans-Bold.ttf", 22),
    ("sans-bold", "DejaVuSans-Bold.ttf", 28),
    ("mono", "DejaVuSansMono.ttf", 13),
    ("mono-bold", "DejaVuSansMono-Bold.ttf", 13),
]

RANGES = [
    (0x20, 0x7E),     # ASCII
    (0xA0, 0xFF),     # Latin-1
    (0x100, 0x17F),   # Latin Extended-A
    (0x180, 0x24F),   # Latin Extended-B
    (0x2BB, 0x2DD),   # modifier letters (ʻ ˆ ˜...)
    (0x370, 0x3FF),   # Greek
    (0x400, 0x45F),   # Cyrillic
    (0x1E00, 0x1EFF), # Latin Extended Additional (IAST, Vietnamese)
    (0x2010, 0x205E), # general punctuation
    (0x20A0, 0x20BF), # currencies
    (0x2100, 0x2122), # letterlike (™, ℃...)
    (0x2190, 0x21FF), # arrows
    (0x2200, 0x2264), # math operators
    (0x2500, 0x257F), # box drawing (for the terminal)
    (0x25A0, 0x25FF), # shapes
    (0x2600, 0x2638), # misc symbols
    (0x2713, 0x2718), # check marks
    (0xFFFD, 0xFFFD), # replacement character
]


def codepoints():
    for start, end in RANGES:
        yield from range(start, end + 1)


def main():
    directory, output = sys.argv[1], sys.argv[2]
    faces = []
    for name, file, size in FACES:
        font = ImageFont.truetype(f"{directory}/{file}", size)
        ascent, descent = font.getmetrics()
        # What a character the font doesn't have looks like (.notdef).
        missing = font.getmask2("", mode="L", anchor="ls")
        missing_bytes = bytes(missing[0])
        glyphs = []
        for cp in codepoints():
            ch = chr(cp)
            mask, (left, top) = font.getmask2(ch, mode="L", anchor="ls")
            width, height = mask.size
            data = bytes(mask)
            if cp != 0x20 and cp != 0xA0 and cp != 0xFFFD and data == missing_bytes:
                continue  # not in this font
            if width == 0 or height == 0:
                width = height = 0
                data = b""
            advance = round(font.getlength(ch) * 64)
            glyphs.append((cp, left, top, width, height, advance, data))
        faces.append((name, size, ascent, descent, glyphs))
        print(f"{name} {size}px: {len(glyphs)} characters", file=sys.stderr)

    header_size = 8 + len(faces) * (16 + 2 + 2 + 2 + 2 + 4 + 4)
    glyph_tables_size = sum(len(f[4]) * 20 for f in faces)
    bitmap_at = header_size + glyph_tables_size

    out = bytearray(struct.pack("<II", 0x314E4642, len(faces)))
    glyph_at = header_size
    for name, size, ascent, descent, glyphs in faces:
        out += struct.pack("<16sHhhHII", name.encode(), size, ascent, descent, ascent + descent,
                           len(glyphs), glyph_at)
        glyph_at += len(glyphs) * 20

    bitmaps = bytearray()
    for name, size, ascent, descent, glyphs in faces:
        for cp, left, top, width, height, advance, data in glyphs:
            out += struct.pack("<IhhHHII", cp, left, top, width, height, advance, bitmap_at + len(bitmaps))
            bitmaps += data
    out += bitmaps

    with open(output, "wb") as f:
        f.write(out)
    print(f"{output}: {len(out) // 1024} KiB", file=sys.stderr)


if __name__ == "__main__":
    main()
