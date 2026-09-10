#ifndef VOW_SHA256_H
#define VOW_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4) over bytes. Pure, no allocation, no I/O. Used
   to verify import pins and lockfile entries. */

typedef struct {
  uint32_t h[8];
  uint64_t total;
  unsigned char block[64];
  size_t nblock;
} Sha256;

/* Contract: prepares a digest state. No failure modes. */
void sha256_init(Sha256 *s);

/* Contract: feeds bytes. No failure modes. */
void sha256_update(Sha256 *s, const unsigned char *p, size_t n);

/* Contract: finalizes into out[32]. No failure modes. */
void sha256_final(Sha256 *s, unsigned char out[32]);

/* Contract: writes the lowercase hex form (64 chars plus NUL) of the
   digest of p[0..n) into out[65]. No failure modes. */
void sha256_hex(const unsigned char *p, size_t n, char out[65]);

#endif
