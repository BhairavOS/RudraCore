#!/bin/sh
# Tests the image decoders (userspace/kapalaframe/src/image.cpp) on the build
# machine against ImageMagick: PNG in every color type and bit depth,
# interlaced PNG, JPEG with different chroma subsampling and restart
# markers, grayscale JPEG, and GIF (with transparency, and interlaced).
# Run inside the toolchain: .\bhairav.ps1 shell, then sh scripts/image-test.sh
set -eu

OUT=build/image-test
mkdir -p "$OUT"
clang++ -std=c++23 -O2 -Iuserspace/kapalaframe/include -fno-exceptions \
    scripts/image-test.cpp userspace/kapalaframe/src/image.cpp -o "$OUT/image-test"

# A colorful source picture with soft and hard edges and transparency.
convert -size 61x43 gradient:'#ff9933-#3b0d2e' \
    -fill '#1a55c8' -draw 'circle 30,21 30,6' \
    -fill none -stroke white -draw 'line 0,0 60,42' -depth 8 "$OUT/source.png"
convert "$OUT/source.png" -alpha set -channel A -fx 'i/w' +channel -depth 8 "$OUT/source-alpha.png"

failed=0
check() { # file, tolerance
    convert -define jpeg:fancy-upsampling=off "$1" -alpha on -depth 8 rgba:"$1.rgba"
    size=$(identify -format '%w %h' "$1[0]")
    "$OUT/image-test" "$1" "$1.rgba" $size "$2" || failed=1
}

convert "$OUT/source-alpha.png" -define png:color-type=6 "$OUT/rgba8.png" && check "$OUT/rgba8.png" 0
convert "$OUT/source.png" -define png:color-type=2 "$OUT/rgb8.png" && check "$OUT/rgb8.png" 0
convert "$OUT/source.png" -depth 16 -define png:color-type=2 "$OUT/rgb16.png" && check "$OUT/rgb16.png" 1
convert "$OUT/source.png" -colorspace gray -define png:color-type=0 "$OUT/gray8.png" && check "$OUT/gray8.png" 0
convert "$OUT/source.png" -colorspace gray -depth 4 -define png:color-type=0 -define png:bit-depth=4 "$OUT/gray4.png" && check "$OUT/gray4.png" 0
convert "$OUT/source-alpha.png" -colorspace gray -define png:color-type=4 "$OUT/graya.png" && check "$OUT/graya.png" 0
convert "$OUT/source.png" -colors 16 -define png:color-type=3 "$OUT/palette.png" && check "$OUT/palette.png" 0
convert "$OUT/source.png" -interlace PNG "$OUT/interlaced.png" && check "$OUT/interlaced.png" 0
convert "$OUT/source.png" -quality 92 -sampling-factor 4:4:4 "$OUT/j444.jpg" && check "$OUT/j444.jpg" 6
convert "$OUT/source.png" -quality 85 -sampling-factor 4:2:0 "$OUT/j420.jpg" && check "$OUT/j420.jpg" 6
convert "$OUT/source.png" -quality 85 -sampling-factor 4:2:2 -define jpeg:restart-interval=1 "$OUT/j422r.jpg" && check "$OUT/j422r.jpg" 6
convert "$OUT/source.png" -colorspace gray -quality 90 "$OUT/jgray.jpg" && check "$OUT/jgray.jpg" 6
convert "$OUT/source-alpha.png" -colors 32 "$OUT/anim.gif" && check "$OUT/anim.gif" 0
convert "$OUT/source.png" -colors 64 -interlace GIF "$OUT/interlaced.gif" && check "$OUT/interlaced.gif" 0

if [ "$failed" = 0 ]; then echo "image test: all decoders match ImageMagick"; else echo "image test: FAILED"; exit 1; fi
