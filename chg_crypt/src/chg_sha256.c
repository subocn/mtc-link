/* 作者：苏勃(subo)
 * 紧凑 SHA-256 与 HMAC-SHA256。工作区由调用方持有，函数内不分配内存。
 */
#include "chg_crypt.h"

#include <string.h>

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha_init(ChgSha256 *c)
{
    c->s[0] = 0x6a09e667u;
    c->s[1] = 0xbb67ae85u;
    c->s[2] = 0x3c6ef372u;
    c->s[3] = 0xa54ff53au;
    c->s[4] = 0x510e527fu;
    c->s[5] = 0x9b05688cu;
    c->s[6] = 0x1f83d9abu;
    c->s[7] = 0x5be0cd19u;
    c->bits = 0;
    c->n = 0;
}

static void sha_block(ChgSha256 *c, const uint8_t *blk, uint32_t *w)
{
    uint32_t a, b, d, e, f, g, h, cc;
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)blk[i * 4u] << 24) | ((uint32_t)blk[i * 4u + 1u] << 16) |
               ((uint32_t)blk[i * 4u + 2u] << 8) | (uint32_t)blk[i * 4u + 3u];
    }
    a = c->s[0];
    b = c->s[1];
    cc = c->s[2];
    d = c->s[3];
    e = c->s[4];
    f = c->s[5];
    g = c->s[6];
    h = c->s[7];
    for (i = 0; i < 64u; i++) {
        uint32_t wi, s0, s1, ch, maj, t1, t2;
        if (i >= 16u) {
            uint32_t x15 = w[(i - 15u) & 15u];
            uint32_t x2 = w[(i - 2u) & 15u];
            s0 = rotr(x15, 7) ^ rotr(x15, 18) ^ (x15 >> 3);
            s1 = rotr(x2, 17) ^ rotr(x2, 19) ^ (x2 >> 10);
            w[i & 15u] = w[i & 15u] + s0 + w[(i - 7u) & 15u] + s1;
        }
        wi = w[i & 15u];
        s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        ch = (e & f) ^ ((~e) & g);
        t1 = h + s1 + ch + K[i] + wi;
        s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->s[0] += a;
    c->s[1] += b;
    c->s[2] += cc;
    c->s[3] += d;
    c->s[4] += e;
    c->s[5] += f;
    c->s[6] += g;
    c->s[7] += h;
}

static void sha_update(ChgSha256 *c, const uint8_t *p, size_t n, uint32_t *w)
{
    size_t i;
    c->bits += (uint64_t)n * 8u;
    for (i = 0; i < n; i++) {
        c->buf[c->n++] = p[i];
        if (c->n == 64u) {
            sha_block(c, c->buf, w);
            c->n = 0;
        }
    }
}

static void sha_final(ChgSha256 *c, uint8_t out[32], uint32_t *w)
{
    uint8_t i;
    uint8_t n = c->n;
    c->buf[n++] = 0x80u;
    if (n > 56u) {
        while (n < 64u) {
            c->buf[n++] = 0;
        }
        sha_block(c, c->buf, w);
        n = 0;
    }
    while (n < 56u) {
        c->buf[n++] = 0;
    }
    for (i = 0; i < 8u; i++) {
        c->buf[63u - i] = (uint8_t)(c->bits >> (8u * i));
    }
    sha_block(c, c->buf, w);
    for (i = 0; i < 8u; i++) {
        out[i * 4u] = (uint8_t)(c->s[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(c->s[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(c->s[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)c->s[i];
    }
}

void chg_sha256(ChgMacWork *w, const uint8_t *msg, size_t n, uint8_t out[32])
{
    if (w == 0 || out == 0 || (n > 0u && msg == 0)) {
        return;
    }
    sha_init(&w->sha);
    sha_update(&w->sha, msg, n, w->w);
    sha_final(&w->sha, out, w->w);
}

void chg_hmac_sha256(ChgMacWork *w, const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    size_t i;
    if (w == 0 || out == 0 || (key_len > 0u && key == 0) || (msg_len > 0u && msg == 0)) {
        return;
    }
    memset(w->pad, 0, sizeof w->pad);
    if (key_len > 64u) {
        sha_init(&w->sha);
        sha_update(&w->sha, key, key_len, w->w);
        sha_final(&w->sha, w->dig, w->w);
        memcpy(w->pad, w->dig, 32u);
    } else if (key_len > 0u) {
        memcpy(w->pad, key, key_len);
    }
    for (i = 0; i < 64u; i++) {
        w->pad[i] ^= 0x36u;
    }
    sha_init(&w->sha);
    sha_update(&w->sha, w->pad, 64u, w->w);
    sha_update(&w->sha, msg, msg_len, w->w);
    sha_final(&w->sha, w->dig, w->w);
    for (i = 0; i < 64u; i++) {
        w->pad[i] ^= (uint8_t)(0x36u ^ 0x5cu);
    }
    sha_init(&w->sha);
    sha_update(&w->sha, w->pad, 64u, w->w);
    sha_update(&w->sha, w->dig, 32u, w->w);
    sha_final(&w->sha, out, w->w);
}
