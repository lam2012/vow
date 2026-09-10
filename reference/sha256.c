#include "sha256.h"

/* First 32 bits of the fractional parts of the cube roots of the
   first 64 primes (FIPS 180-4, Section 4.2.2). Verified at review
   time against the system sha256sum on fixed vectors. */
static const uint32_t kK[64] = {
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

static uint32_t rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

static void compress(Sha256 *s, const unsigned char *p) {
  uint32_t w[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  uint32_t t1;
  uint32_t t2;
  size_t i;
  size_t j;
  for (i = 0; i < 16; i++) {
    j = i * 4u;
    w[i] = ((uint32_t)p[j] << 24u) | ((uint32_t)p[j + 1] << 16u) |
           ((uint32_t)p[j + 2] << 8u) | (uint32_t)p[j + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7u) ^ rotr(w[i - 15], 18u) ^
                  (w[i - 15] >> 3u);
    uint32_t s1 = rotr(w[i - 2], 17u) ^ rotr(w[i - 2], 19u) ^
                  (w[i - 2] >> 10u);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0];
  b = s->h[1];
  c = s->h[2];
  d = s->h[3];
  e = s->h[4];
  f = s->h[5];
  g = s->h[6];
  h = s->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t big0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t big1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
    uint32_t ch = (e & f) ^ ((~e) & g);
    t1 = h + big1 + ch + kK[i] + w[i];
    t2 = big0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

void sha256_init(Sha256 *s) {
  s->h[0] = 0x6a09e667u;
  s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u;
  s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu;
  s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu;
  s->h[7] = 0x5be0cd19u;
  s->total = 0;
  s->nblock = 0;
}

void sha256_update(Sha256 *s, const unsigned char *p, size_t n) {
  size_t i = 0;
  s->total += (uint64_t)n;
  while (i < n) {
    s->block[s->nblock] = p[i];
    s->nblock += 1;
    i += 1;
    if (s->nblock == 64) {
      compress(s, s->block);
      s->nblock = 0;
    }
  }
}

void sha256_final(Sha256 *s, unsigned char out[32]) {
  uint64_t bits = s->total * 8u;
  unsigned char pad = 0x80;
  unsigned char zero = 0x00;
  unsigned char lenbuf[8];
  size_t i;
  int k;
  sha256_update(s, &pad, 1);
  while (s->nblock != 56) {
    sha256_update(s, &zero, 1);
  }
  for (k = 7; k >= 0; k--) {
    lenbuf[7 - (size_t)k] = (unsigned char)((bits >> ((unsigned)k * 8u)) &
                                            0xFFu);
  }
  sha256_update(s, lenbuf, 8);
  for (i = 0; i < 8; i++) {
    out[i * 4u] = (unsigned char)((s->h[i] >> 24u) & 0xFFu);
    out[i * 4u + 1] = (unsigned char)((s->h[i] >> 16u) & 0xFFu);
    out[i * 4u + 2] = (unsigned char)((s->h[i] >> 8u) & 0xFFu);
    out[i * 4u + 3] = (unsigned char)(s->h[i] & 0xFFu);
  }
}

void sha256_hex(const unsigned char *p, size_t n, char out[65]) {
  static const char digits[] = "0123456789abcdef";
  unsigned char sum[32];
  Sha256 s;
  size_t i;
  sha256_init(&s);
  sha256_update(&s, p, n);
  sha256_final(&s, sum);
  for (i = 0; i < 32; i++) {
    out[i * 2u] = digits[(sum[i] >> 4u) & 0xFu];
    out[i * 2u + 1] = digits[sum[i] & 0xFu];
  }
  out[64] = '\0';
}
