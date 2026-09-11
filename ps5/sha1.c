/* SHA-1 (FIPS 180-1) for input of any length, used to check that the ROM the
 * player supplied is the one the asset map was made against.
 *
 * The copy in ps5link, which this replaced, only takes short input - it exists
 * to hash symbol names for NIDs - and hashed an 8 MB ROM to all zeroes. This
 * one works through the input a block at a time and pads only the tail. */
#include "sha1.h"
#include <string.h>

static uint32_t rol32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++, p += 4)
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const uint8_t *data, size_t len, uint8_t digest_out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    size_t whole = len - len % 64;
    for (size_t at = 0; at < whole; at += 64)
        sha1_block(h, data + at);

    /* The tail, the 0x80 marker and the bit length: one block or two. */
    uint8_t tail[128];
    size_t rest = len - whole;
    size_t tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
    memcpy(tail, data + whole, rest);
    tail[rest] = 0x80;
    memset(tail + rest + 1, 0, tail_len - rest - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t at = 0; at < tail_len; at += 64)
        sha1_block(h, tail + at);

    for (int i = 0; i < 5; i++) {
        digest_out[i * 4 + 0] = (uint8_t)(h[i] >> 24);
        digest_out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        digest_out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        digest_out[i * 4 + 3] = (uint8_t)(h[i]);
    }
}
