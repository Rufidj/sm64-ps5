/* asset_loader - puts the game's ROM-derived data back into the program from
 * the player's own ROM, so the program itself can be shared without it.
 *
 * After linking, asset_tools/asset_strip.py zeroes every texture and binary
 * asset the build took from the ROM and writes a map: where in the program
 * each one goes, and where in the ROM it comes from - a raw offset, or an
 * offset into one of the ROM's MIO0-compressed segments. This reads that map
 * and the ROM and copies everything back before the game starts.
 *
 * Addresses in the map are the program's own, as linked; the program finds
 * where it was actually loaded through the anchor string below, whose linked
 * address the map also records. Most assets sit in .rodata, which is mapped
 * read-only, so it is made writable for the copy and read-only again after.
 *
 * The file functions used are the ones the program can import: there is no
 * fseek or ftell, so files are read in blocks instead of being measured.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "asset_loader.h"
#include "sha1.h"

#define MAP_PATH     "/app0/sm64_assets.map"
#define ROM_PATH     "/app0/data/sm64_ps5/baserom.us.z64"
#define ROM_SIZE     (8u * 1024u * 1024u)
#define MAP_MAX      (1u * 1024u * 1024u)
#define MAP_VERSION  2u
#define MAP_HEADER   52u               /* magic, six u32 fields, the SHA-1 */
#define RAW          0xFFFFFFFFu       /* entry sources that read the ROM itself: as it is, */
#define RAW_SWAP16   0xFFFFFFFEu       /* with bytes reversed in pairs,                     */
#define RAW_SWAP32   0xFFFFFFFDu       /* or in fours                                       */
#define PAGE         0x4000u

/* The program finds its load address through this: the map holds the address
 * it was linked at. It must stay unique in .rodata. */
__attribute__((used)) static const char s_anchor[] = "SM64-ASSET-ANCHOR";

static const char *s_error = "";

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/* Reads a whole file of at most `max` bytes. A file larger than that reads as
 * max + 1 bytes, which callers can reject. */
static uint8_t *read_whole(const char *path, size_t max, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = malloc(max + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t size = 0;
    while (size < max + 1) {
        size_t got = fread(buf + size, 1, (max + 1 - size) < 65536 ? (max + 1 - size) : 65536, f);
        if (got == 0) break;
        size += got;
    }
    fclose(f);
    *size_out = size;
    return buf;
}

/* Brings a ROM dumped in either of the other two byte orders to .z64's. */
static int normalise_rom(uint8_t *rom, size_t size) {
    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)
        return 0;                                             /* .z64 */
    if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {
        for (size_t i = 0; i + 1 < size; i += 2) {            /* .v64: bytes paired and swapped */
            uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t;
        }
        return 0;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {
        for (size_t i = 0; i + 3 < size; i += 4) {            /* .n64: words reversed */
            uint8_t a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
        return 0;
    }
    return -1;
}

/* Decompresses the MIO0 segment at `at`. NULL if it is not one or is damaged. */
static uint8_t *mio0_decode(const uint8_t *rom, size_t rom_size, uint32_t at, uint32_t *size_out) {
    if ((size_t)at + 16 > rom_size || memcmp(rom + at, "MIO0", 4) != 0) return NULL;
    uint32_t size = be32(rom + at + 4);
    size_t layout = (size_t)at + 16, comp = (size_t)at + be32(rom + at + 8), raw = (size_t)at + be32(rom + at + 12);
    uint8_t *out = malloc(size ? size : 1);
    if (!out) return NULL;
    uint32_t n = 0, bits = 0;
    int nbits = 0;
    while (n < size) {
        if (nbits == 0) {
            if (layout + 4 > rom_size) goto damaged;
            bits = be32(rom + layout);
            layout += 4;
            nbits = 32;
        }
        if (bits & 0x80000000u) {
            if (raw >= rom_size) goto damaged;
            out[n++] = rom[raw++];
        } else {
            if (comp + 2 > rom_size) goto damaged;
            uint32_t v = be16(rom + comp);
            comp += 2;
            uint32_t length = (v >> 12) + 3, back = (v & 0xFFF) + 1;
            if (back > n) goto damaged;
            for (uint32_t i = 0; i < length && n < size; i++, n++) out[n] = out[n - back];
        }
        bits <<= 1;
        nbits--;
    }
    *size_out = size;
    return out;
damaged:
    free(out);
    return NULL;
}

const char *asset_loader_error(void) {
    return s_error;
}

/* The part that does not depend on the console: checks the map against the
 * ROM (normalising the ROM's byte order in place) and copies every asset to
 * base + its linked address. When `protect` is set, the .rodata range is made
 * writable around the copy. Kept apart so that a host test can run it over a
 * copy of the program laid out in a buffer. */
int asset_loader_restore_into(const uint8_t *map, size_t map_size, uint8_t *rom, size_t rom_size,
                              int use_anchor, uintptr_t base, int protect) {
    if (map_size < MAP_HEADER || memcmp(map, "SM64AMAP", 8) != 0 || le32(map + 8) != MAP_VERSION) {
        s_error = "sm64: sm64_assets.map is not valid";
        return -1;
    }
    uint32_t entry_count = le32(map + 12), segment_count = le32(map + 16), anchor = le32(map + 20);
    uint32_t rodata_addr = le32(map + 24), rodata_size = le32(map + 28);
    const uint8_t *rom_sha1 = map + 32;
    if ((use_anchor && anchor == RAW) || (uint64_t)MAP_HEADER + 4ull * segment_count + 16ull * entry_count > map_size) {
        s_error = "sm64: sm64_assets.map is incomplete";
        return -1;
    }
    const uint8_t *segments = map + MAP_HEADER;
    const uint8_t *entries = segments + 4u * segment_count;

    if (rom_size != ROM_SIZE || normalise_rom(rom, rom_size) != 0) {
        s_error = "sm64: data/sm64_ps5/baserom.us.z64 is not an SM64 ROM";
        return -1;
    }
    uint8_t digest[20];
    sha1(rom, rom_size, digest);
    if (memcmp(digest, rom_sha1, 20) != 0) {
        s_error = "sm64: the ROM is not the US version of Super Mario 64";
        return -1;
    }

    if (use_anchor) base = (uintptr_t)s_anchor - anchor;
    uintptr_t ro_start = (base + rodata_addr) & ~(uintptr_t)(PAGE - 1);
    uintptr_t ro_end = (base + rodata_addr + rodata_size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    if (protect && mprotect((void *)ro_start, ro_end - ro_start, PROT_READ | PROT_WRITE) != 0) {
        s_error = "sm64: .rodata could not be made writable";
        return -1;
    }

    /* Raw ROM data first - as it is, or with its bytes reversed in pairs or in
     * fours where the build rewrote it little-endian - then each compressed
     * segment, decompressed once. */
    int result = 0;
    for (uint32_t i = 0; i < entry_count && result == 0; i++) {
        const uint8_t *e = entries + 16u * i;
        uint32_t source = le32(e + 8), length = le32(e + 4), offset = le32(e + 12);
        if (source != RAW && source != RAW_SWAP16 && source != RAW_SWAP32) continue;
        if ((uint64_t)offset + length > rom_size) { s_error = "sm64: the map points past the end of the ROM"; result = -1; break; }
        uint8_t *dst = (uint8_t *)(base + le32(e));
        const uint8_t *src = rom + offset;
        if (source == RAW) {
            memcpy(dst, src, length);
        } else if (source == RAW_SWAP16) {
            for (uint32_t b = 0; b + 1 < length; b += 2) {
                dst[b] = src[b + 1];
                dst[b + 1] = src[b];
            }
        } else {
            for (uint32_t b = 0; b + 3 < length; b += 4) {
                dst[b] = src[b + 3];
                dst[b + 1] = src[b + 2];
                dst[b + 2] = src[b + 1];
                dst[b + 3] = src[b];
            }
        }
    }
    for (uint32_t s = 0; s < segment_count && result == 0; s++) {
        uint32_t seg_size = 0;
        uint8_t *seg = mio0_decode(rom, rom_size, le32(segments + 4u * s), &seg_size);
        if (!seg) { s_error = "sm64: a compressed segment of the ROM is damaged"; result = -1; break; }
        for (uint32_t i = 0; i < entry_count; i++) {
            const uint8_t *e = entries + 16u * i;
            uint32_t length = le32(e + 4), offset = le32(e + 12);
            if (le32(e + 8) != s) continue;
            if ((uint64_t)offset + length > seg_size) { s_error = "sm64: the map points past the end of a segment"; result = -1; break; }
            memcpy((void *)(base + le32(e)), seg + offset, length);
        }
        free(seg);
    }

    if (protect) mprotect((void *)ro_start, ro_end - ro_start, PROT_READ);
    return result;
}

int asset_loader_restore(void) {
    size_t map_size = 0, rom_size = 0;
    uint8_t *map = read_whole(MAP_PATH, MAP_MAX, &map_size);
    if (!map) return 1;                                       /* a build that still carries its assets */

    int result = -1;
    uint8_t *rom = read_whole(ROM_PATH, ROM_SIZE, &rom_size);
    if (!rom)
        s_error = "sm64: the ROM is missing from data/sm64_ps5/baserom.us.z64";
    else
        result = asset_loader_restore_into(map, map_size, rom, rom_size, 1, 0, 1);

    free(rom);
    free(map);
    return result;
}
