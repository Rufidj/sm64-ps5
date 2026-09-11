/* hd_textures - the game's textures replaced by an sm64ex texture pack.
 *
 * The player puts the pack's images in data/sm64_ps5/texturas_hd/ - either the
 * pack's gfx folder itself or what is inside it. The program carries an index,
 * sm64_hd_textures.idx, that files each of the game's textures under a hash of
 * its bytes with the path of the image that replaces it (hd_tools/hd_index.py).
 * As the renderer imports a texture it asks here: the texture's bytes are
 * hashed, looked up, and the image is read and decoded - stb_image, PNG only.
 *
 * Images are decoded as they are first needed, which is when the renderer
 * imports the texture, and kept by the renderer from then on. A texture whose
 * image is missing or unreadable is remembered, so it is not tried again.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hd_textures.h"
#include "ps5gpu.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
/* Its thread-local state would need the runtime's emulated TLS, which the
 * linker's catalogue does not have; images are only decoded on one thread. */
#define STBI_NO_THREAD_LOCALS
#include "third_party/stb_image.h"

#define INDEX_FILE "/app0/sm64_hd_textures.idx"
#define INDEX_HEADER 24
#define MAX_FAILED 4096

static const char *const kRoots[] = {
    "/app0/data/sm64_ps5/texturas_hd/gfx/",
    "/app0/data/sm64_ps5/texturas_hd/",
};

typedef struct {
    uint64_t hash;
    uint32_t size;
    uint32_t path;
} Entry;

static uint8_t *s_index;
static const Entry *s_entries;
static uint32_t s_count;
static const char *s_paths;
static uint32_t s_paths_size;
static const char *s_root;

static uint64_t s_failed[MAX_FAILED];
static int s_failed_count;

static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ULL; }
    return h;
}

/* A whole file, read in growing blocks: the console's libc here has no fseek. */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t capacity = 1 << 16, n = 0;
    uint8_t *buf = malloc(capacity);
    while (buf) {
        if (n == capacity) {
            uint8_t *bigger = realloc(buf, capacity * 2);
            if (!bigger) { free(buf); buf = NULL; break; }
            buf = bigger;
            capacity *= 2;
        }
        size_t got = fread(buf + n, 1, capacity - n, f);
        if (got == 0) break;
        n += got;
    }
    fclose(f);
    *size = n;
    return buf;
}

void hd_textures_init(void) {
    size_t n = 0;
    uint8_t *index = read_file(INDEX_FILE, &n);
    if (!index) return;
    if (n < INDEX_HEADER || memcmp(index, "SM64HDIX", 8) != 0 || le32(index + 8) != 1) {
        free(index);
        return;
    }
    uint32_t count = le32(index + 12), paths_size = le32(index + 16);
    if ((uint64_t)INDEX_HEADER + 16ull * count + paths_size > n) {
        free(index);
        return;
    }
    s_index = index;
    s_count = count;
    s_entries = (const Entry *)(index + INDEX_HEADER);
    s_paths = (const char *)(index + INDEX_HEADER + 16 * count);
    s_paths_size = paths_size;

    /* A pack is there if its top-level folders are. */
    for (size_t r = 0; r < sizeof kRoots / sizeof kRoots[0]; r++) {
        char probe[256];
        size_t len = strlen(kRoots[r]);
        memcpy(probe, kRoots[r], len);
        memcpy(probe + len, "actors", 7);
        struct stat st;
        if (stat(probe, &st) == 0) { s_root = kRoots[r]; break; }
    }
}

bool hd_textures_available(void) {
    return s_root != NULL && s_count > 0;
}

static const Entry *find(uint64_t hash, uint32_t size) {
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_entries[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < s_count && s_entries[lo].hash == hash; lo++)
        if (s_entries[lo].size == size) return &s_entries[lo];
    return NULL;
}

static bool failed_before(uint64_t hash) {
    for (int i = 0; i < s_failed_count; i++)
        if (s_failed[i] == hash) return true;
    return false;
}

static void remember_failure(uint64_t hash) {
    if (s_failed_count < MAX_FAILED) s_failed[s_failed_count++] = hash;
}

uint8_t *hd_texture_load(const uint8_t *data, uint32_t size, int *width, int *height) {
    if (!hd_textures_available() || !data || size == 0) return NULL;
    uint64_t hash = fnv1a64(data, size);
    const Entry *e = find(hash, size);
    if (!e || e->path >= s_paths_size || failed_before(hash)) return NULL;

    char path[512];
    size_t root_len = strlen(s_root), name_len = 0;
    while (e->path + name_len < s_paths_size && s_paths[e->path + name_len]) name_len++;
    if (root_len + name_len + 1 > sizeof path) return NULL;
    memcpy(path, s_root, root_len);
    memcpy(path + root_len, s_paths + e->path, name_len);
    path[root_len + name_len] = 0;

    size_t png_size = 0;
    uint8_t *png = read_file(path, &png_size);
    if (!png) { remember_failure(hash); return NULL; }
    int channels = 0;
    uint8_t *pixels = stbi_load_from_memory(png, (int)png_size, width, height, &channels, 4);
    free(png);
    if (!pixels || *width <= 0 || *height <= 0) {
        if (pixels) stbi_image_free(pixels);
        remember_failure(hash);
        return NULL;
    }
    return pixels;
}

void hd_texture_free(uint8_t *pixels) {
    stbi_image_free(pixels);
}
