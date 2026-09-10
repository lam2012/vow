#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void arena_init(Arena *a, unsigned char *base, size_t cap) {
  a->base = base;
  a->cap = cap;
  a->used = 0;
}

/* Every allocation is aligned for any fundamental type, so structs
   holding pointers never sit at an odd offset (this is undefined
   behavior on strict-alignment targets such as ARM64 macOS). */
size_t arena_align_up(size_t n) {
  size_t a = (size_t)_Alignof(max_align_t);
  return (n + a - 1u) & ~(a - 1u);
}

void *arena_alloc(Arena *a, size_t n) {
  void *out;
  n = arena_align_up(n);
  if (n == 0 || n > a->cap || a->used > a->cap - n) {
    fprintf(stderr, "vow0: arena exhausted (E5006 resource limit breach)\n");
    exit(5);
  }
  out = (void *)(a->base + a->used);
  a->used += n;
  memset(out, 0, n);
  return out;
}
