// Checks Kapalaframe's image decoders on the build machine: decodes files and
// compares every pixel with what ImageMagick decodes (rgba:- dumps).
//
//   scripts/image-test.sh   (builds this and runs it on generated images)
//
// Usage: image-test <image> <reference.rgba> <width> <height> <tolerance>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <kapala/image.h>

static unsigned char* readFile(char const* path, unsigned long& length) {
    FILE* f = fopen(path, "rb");
    if (not f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    length = static_cast<unsigned long>(ftell(f));
    fseek(f, 0, SEEK_SET);
    auto* data = static_cast<unsigned char*>(malloc(length));
    if (fread(data, 1, length, f) != length)
        length = 0;
    fclose(f);
    return data;
}

int main(int argc, char** argv) {
    if (argc != 6)
        return 2;
    unsigned long length = 0, refLength = 0;
    unsigned char* data = readFile(argv[1], length);
    unsigned char* ref = readFile(argv[2], refLength);
    int width = atoi(argv[3]), height = atoi(argv[4]), tolerance = atoi(argv[5]);
    Kapala::Image image;
    if (not data or not Kapala::Image::decode(data, length, image)) {
        printf("FAIL %s: didn't decode\n", argv[1]);
        return 1;
    }
    if (image.width != width or image.height != height or refLength != static_cast<unsigned long>(width) * height * 4) {
        printf("FAIL %s: size %dx%d\n", argv[1], image.width, image.height);
        return 1;
    }
    int worst = 0;
    for (unsigned long i = 0; i < refLength; i++) {
        int d = abs(image.pixels[i] - ref[i]);
        if (d > worst)
            worst = d;
    }
    printf("%s %s: %dx%d, largest difference %d\n", worst <= tolerance ? "ok  " : "FAIL", argv[1], width, height,
           worst);
    return worst <= tolerance ? 0 : 1;
}
