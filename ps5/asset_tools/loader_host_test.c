/* loader_host_test - runs asset_loader.c's restore on the PC, over a copy of
 * the program laid out in a buffer at its linked addresses, and checks the
 * result against the original program byte for byte.
 *
 *   cc -O2 -I.. -o loader_host_test loader_host_test.c ../asset_loader.c ../sha1.c
 *   ./loader_host_test <original elf> <stripped elf> <map> <rom>
 *
 * Exercises everything the console runs except the file reading, the anchor
 * and mprotect: the map format, the ROM check and byte-order handling, the MIO0
 * decoder and every copy.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int asset_loader_restore_into(const uint8_t *map, size_t map_size, uint8_t *rom, size_t rom_size,
                              int use_anchor, uintptr_t base, int protect);
const char *asset_loader_error(void);

typedef struct { uint64_t vaddr, offset, size; } Segment;

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size ? *size : 1);
    if (fread(buf, 1, *size, f) != *size) { perror(path); exit(2); }
    fclose(f);
    return buf;
}

static int load_segments(const uint8_t *elf, Segment *out, int max) {
    uint64_t phoff;
    uint16_t entsize, count;
    memcpy(&phoff, elf + 0x20, 8);
    memcpy(&entsize, elf + 0x36, 2);
    memcpy(&count, elf + 0x38, 2);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        const uint8_t *ph = elf + phoff + (uint64_t)i * entsize;
        uint32_t type;
        memcpy(&type, ph, 4);
        if (type != 1) continue;
        memcpy(&out[n].offset, ph + 8, 8);
        memcpy(&out[n].vaddr, ph + 16, 8);
        memcpy(&out[n].size, ph + 32, 8);
        n++;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <original elf> <stripped elf> <map> <rom>\n", argv[0]);
        return 2;
    }
    size_t orig_size, strip_size, map_size, rom_size;
    uint8_t *orig = slurp(argv[1], &orig_size);
    uint8_t *strip = slurp(argv[2], &strip_size);
    uint8_t *map = slurp(argv[3], &map_size);
    uint8_t *rom = slurp(argv[4], &rom_size);
    if (orig_size != strip_size) { fprintf(stderr, "the two ELFs differ in size\n"); return 1; }

    Segment segs[16];
    int nsegs = load_segments(strip, segs, 16);
    uint64_t top = 0;
    for (int i = 0; i < nsegs; i++)
        if (segs[i].vaddr + segs[i].size > top) top = segs[i].vaddr + segs[i].size;

    /* The program as the console would have it: every loadable segment copied
     * to its address in one buffer. */
    uint8_t *image = calloc(1, top);
    for (int i = 0; i < nsegs; i++)
        memcpy(image + segs[i].vaddr, strip + segs[i].offset, segs[i].size);

    if (asset_loader_restore_into(map, map_size, rom, rom_size, 0, (uintptr_t)image, 0) != 0) {
        fprintf(stderr, "restore failed: %s\n", asset_loader_error());
        return 1;
    }

    size_t differing = 0;
    for (int i = 0; i < nsegs; i++)
        for (uint64_t b = 0; b < segs[i].size; b++)
            if (image[segs[i].vaddr + b] != orig[segs[i].offset + b]) differing++;
    if (differing) {
        fprintf(stderr, "restored image differs from the original in %zu bytes\n", differing);
        return 1;
    }
    printf("restored image matches the original in all %d loadable segments\n", nsegs);
    return 0;
}
