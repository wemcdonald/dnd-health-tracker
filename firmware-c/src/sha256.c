/*
 * SHA-256 — vendored public-domain implementation.
 *
 * Derived from the B-Con / Brad Conte crypto-algorithms repository:
 *   https://github.com/B-Con/crypto-algorithms
 * Original author: Brad Conte (brad@bradconte.com)
 * Released into the public domain (no copyright claimed).
 *
 * Adapted for this project:
 *   - Struct fields renamed to match project API:
 *       datalen → buflen  (size_t, bytes currently in buf)
 *       bitlen  stays     (uint64_t, total message bits)
 *       state   stays     (uint32_t[8])
 *       data    → buf     (uint8_t[64])
 *   - Function signatures match sha256.h (sha256_init / sha256_update /
 *     sha256_final / sha256_hex).
 *   - All byte assembly done explicitly; no host-endian memcpy of words.
 *   - Passes FIPS-180-4 test vectors for "", "abc".
 */

#include "sha256.h"
#include <string.h>

/* ---- FIPS-180-4 constants ---- */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x,  2u) ^ ROTR(x, 13u) ^ ROTR(x, 22u))
#define EP1(x) (ROTR(x,  6u) ^ ROTR(x, 11u) ^ ROTR(x, 25u))
#define SIG0(x) (ROTR(x,  7u) ^ ROTR(x, 18u) ^ ((x) >>  3u))
#define SIG1(x) (ROTR(x, 17u) ^ ROTR(x, 19u) ^ ((x) >> 10u))

/* Process one 64-byte block already sitting in c->buf. */
static void sha256_transform(sha256_ctx *c)
{
    uint32_t w[64];
    uint32_t a, b, d, e, f, g, h, tmp1, tmp2;
    uint32_t cc; /* renamed to avoid shadowing 'c' parameter */
    int i;

    /* Build message schedule from big-endian bytes in buf */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)c->buf[i * 4    ] << 24u)
             | ((uint32_t)c->buf[i * 4 + 1] << 16u)
             | ((uint32_t)c->buf[i * 4 + 2] <<  8u)
             | ((uint32_t)c->buf[i * 4 + 3]);
    }
    for (; i < 64; i++) {
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5];  g = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        tmp1 = h + EP1(e) + CH(e, f, g) + K[i] + w[i];
        tmp2 = EP0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + tmp1;
        d = cc; cc = b; b = a; a = tmp1 + tmp2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] +=  g; c->state[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->buflen = 0;
    c->bitlen  = 0;
    /* FIPS-180-4 initial hash values (first 32 bits of fractional parts of
     * square roots of the first 8 primes). */
    c->state[0] = 0x6a09e667u;
    c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u;
    c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu;
    c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu;
    c->state[7] = 0x5be0cd19u;
}

void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64u) {
            sha256_transform(c);
            c->bitlen += 512u;
            c->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    size_t i = c->buflen;

    /* Pad with 0x80 then zero bytes */
    c->buf[i++] = 0x80u;
    if (i > 56u) {
        /* Not enough room for the 8-byte length field in this block;
         * zero-fill to end of block, transform, then start a new block. */
        while (i < 64u) c->buf[i++] = 0x00u;
        sha256_transform(c);
        i = 0;
    }
    while (i < 56u) c->buf[i++] = 0x00u;

    /* Append total message length in BITS as 64-bit big-endian */
    c->bitlen += (uint64_t)c->buflen * 8u;  /* bits from the partial block */
    c->buf[56] = (uint8_t)(c->bitlen >> 56u);
    c->buf[57] = (uint8_t)(c->bitlen >> 48u);
    c->buf[58] = (uint8_t)(c->bitlen >> 40u);
    c->buf[59] = (uint8_t)(c->bitlen >> 32u);
    c->buf[60] = (uint8_t)(c->bitlen >> 24u);
    c->buf[61] = (uint8_t)(c->bitlen >> 16u);
    c->buf[62] = (uint8_t)(c->bitlen >>  8u);
    c->buf[63] = (uint8_t)(c->bitlen);
    sha256_transform(c);

    /* Output state words big-endian — endian-independent byte assembly */
    for (int j = 0; j < 8; j++) {
        out[j * 4    ] = (uint8_t)(c->state[j] >> 24u);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16u);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >>  8u);
        out[j * 4 + 3] = (uint8_t)(c->state[j]);
    }
}

void sha256_hex(const uint8_t in[32], char out[65])
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = h[in[i] >> 4]; out[i*2+1] = h[in[i] & 0xf]; }
    out[64] = '\0';
}
