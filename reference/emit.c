#include "emit.h"

#include <stdio.h>

#include "check.h"
#include "diag.h"
#include "eval.h"
#include "platform.h"

/* ABI slice: `vow build --emit-dylib` plus the embedded runner.
   The dylib reuses the whole interpreter; the generated stub only
   embeds the entry bytes and the absolute source root. No native
   codegen is claimed. All copies are bounded and fail loudly. */

#define EMIT_PATH_CAP 4096
#define EMIT_FILE_CAP ((size_t)64 * (size_t)1024 * (size_t)1024)
#define EMIT_IO_CAP ((size_t)1024 * (size_t)1024)

static int emit_streq(const char *a, const char *b) {
  size_t i = 0;
  for (;;) {
    if (a[i] != b[i]) {
      return 0;
    }
    if (a[i] == '\0') {
      return 1;
    }
    i += 1;
  }
}

static int emit_ends_vow(const char *name) {
  size_t n = 0;
  while (name[n] != '\0') {
    n += 1;
  }
  return n > 4 && name[n - 4] == '.' && name[n - 3] == 'v' &&
         name[n - 2] == 'o' && name[n - 1] == 'w';
}

/* Basename after the last '/' or '\\'. */
static void emit_basename(const char *path, char *out, size_t cap) {
  size_t last = 0;
  size_t has = 0;
  size_t i = 0;
  size_t k = 0;
  while (path[i] != '\0') {
    if (path[i] == '/' || path[i] == '\\') {
      last = i;
      has = 1;
    }
    i += 1;
  }
  if (has == 0) {
    last = 0;
  } else {
    last += 1;
  }
  while (path[last + k] != '\0' && k + 1 < cap) {
    out[k] = path[last + k];
    k += 1;
  }
  out[k] = '\0';
}

/* Directory part of a path, "." when bare (same rule as the
   checker's source-root derivation). */
static void emit_dirname(const char *path, char *out, size_t cap) {
  size_t len = 0;
  size_t last = 0;
  size_t has = 0;
  size_t n = 0;
  while (path[len] != '\0') {
    if (path[len] == '/' || path[len] == '\\') {
      last = len;
      has = 1;
    }
    len += 1;
  }
  if (has == 0) {
    out[0] = '.';
    out[1] = '\0';
    return;
  }
  while (n < last && n + 1 < cap) {
    out[n] = path[n];
    n += 1;
  }
  out[n] = '\0';
}

static int emit_is_abs(const char *p) {
  return p[0] == '/';
}

/* Symbol body: alphanumerics kept, everything else becomes '_'.
   The "vow_" prefix is added by the caller. */
static int emit_mangle(const char *base, char *out, size_t cap) {
  size_t i = 0;
  size_t k = 0;
  const char *prefix = "vow_";
  while (prefix[i] != '\0' && k + 1 < cap) {
    out[k] = prefix[i];
    i += 1;
    k += 1;
  }
  i = 0;
  while (base[i] != '\0') {
    char c = base[i];
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9');
    if (k + 1 >= cap) {
      return 0;
    }
    out[k] = ok ? c : '_';
    k += 1;
    i += 1;
  }
  out[k] = '\0';
  return k > 4;
}

static int emit_join(char *out, size_t cap, const char *a,
                     const char *b) {
  size_t i = 0;
  size_t k = 0;
  while (a[i] != '\0') {
    if (i + 1 >= cap) {
      return 0;
    }
    out[i] = a[i];
    i += 1;
  }
  if (i == 0 || out[i - 1] != '/') {
    if (i + 1 >= cap) {
      return 0;
    }
    out[i] = '/';
    i += 1;
  }
  while (b[k] != '\0') {
    if (i + 1 >= cap) {
      return 0;
    }
    out[i] = b[k];
    i += 1;
    k += 1;
  }
  out[i] = '\0';
  return 1;
}

/* Lexical canonicalization for absolute paths: collapses "/./"
   and "seg/../" (leading ".." clamps at "/", kernel behavior),
   drops duplicate and trailing separators. Byte-oriented, no
   syscalls, deterministic. Absolute input only; anything else
   fails loudly. Needed because the evaluator prefix-matches the
   raw root against normalized resolutions (an uncanonical root
   would fence out its own tree, e.g. lock.check verifying
   false). */
static int emit_normalize(const char *path, char *out, size_t cap) {
  static size_t starts[1024];
  size_t n = 0;
  size_t r;
  size_t w = 0;
  size_t k = 0;
  if (path[0] != '/') {
    return 0;
  }
  while (path[n] != '\0') {
    if (n + 1 >= cap) {
      return 0;
    }
    out[n] = path[n];
    n += 1;
  }
  out[n] = '\0';
  /* Compaction invariant: w <= r always, so the forward byte copy
     per segment never reads a position already overwritten (future
     reads sit past every written byte). The leading '/' is written
     by the first kept segment, not ahead of time. */
  r = 1;
  while (r <= n) {
    size_t s = r;
    size_t slen;
    while (s < n && out[s] != '/') {
      s += 1;
    }
    slen = s - r;
    if (slen == 0 || (slen == 1 && out[r] == '.')) {
      /* skip */
    } else if (slen == 2 && out[r] == '.' && out[r + 1] == '.') {
      if (k > 0) {
        k -= 1;
        w = starts[k];
      }
    } else {
      size_t t;
      if (k >= 1024 || w + 1 + slen >= cap) {
        return 0;
      }
      starts[k] = w;
      k += 1;
      out[w] = '/';
      w += 1;
      for (t = 0; t < slen; t++) {
        out[w] = out[r + t];
        w += 1;
      }
    }
    r = s + 1;
  }
  if (k == 0) {
    out[0] = '/';
    out[1] = '\0';
    return 1;
  }
  out[w] = '\0';
  return 1;
}

/* Plain concatenation without a separator (for linker flags). */
static int emit_concat(char *out, size_t cap, const char *a,
                       const char *b) {
  size_t i = 0;
  size_t k = 0;
  while (a[i] != '\0') {
    if (i + 1 >= cap) {
      return 0;
    }
    out[i] = a[i];
    i += 1;
  }
  while (b[k] != '\0') {
    if (i + 1 >= cap) {
      return 0;
    }
    out[i] = b[k];
    i += 1;
    k += 1;
  }
  out[i] = '\0';
  return 1;
}

static int emit_class_exit(const char *code) {
  char c = code[1];
  if (c == '2') {
    return 2;
  }
  if (c == '3') {
    return 3;
  }
  if (c == '4') {
    return 4;
  }
  if (c == '5') {
    return 5;
  }
  return 1;
}

int emit_run_embedded(const unsigned char *src, size_t len,
                      const char *display, const char *root) {
  static unsigned char arena_buf[8 * 1024 * 1024];
  Arena arena;
  Checker chk;
  Module *entry = NULL;
  size_t i;
  arena_init(&arena, arena_buf, sizeof(arena_buf));
  check_init(&chk, &arena);
  ev_memo_reset();
  if (!check_memory(&chk, display, root, src, len, 1)) {
    fprintf(stderr, "%s:%d:%d: %s %s\n", chk.err_file, chk.err_line,
            chk.err_col, chk.err_code, chk.err_msg);
    return emit_class_exit(chk.err_code);
  }
  for (i = 0; i < chk.nmod; i++) {
    const char *p = chk.modules[i]->path;
    size_t j = 0;
    int same = 1;
    for (;;) {
      if (p[j] != display[j]) {
        same = 0;
        break;
      }
      if (p[j] == '\0') {
        break;
      }
      j += 1;
    }
    if (same) {
      entry = chk.modules[i];
      break;
    }
  }
  if (entry == NULL) {
    fprintf(stderr, "%s:1:1: E5006 resource limit breach\n",
            display);
    return 5;
  }
  {
    Ev ev;
    Decl *d;
    Val *mainv = NULL;
    ev_init(&ev, &chk, entry, chk.root);
    for (d = entry->decls; d != NULL; d = d->next) {
      if (d->kind == D_FUNC && d->name.n == 4 &&
          d->name.p[0] == 'm' && d->name.p[1] == 'a' &&
          d->name.p[2] == 'i' && d->name.p[3] == 'n' &&
          d->nparam == 0) {
        mainv = ev_func_val(&ev, entry, d, d->line, d->col);
        break;
      }
    }
    if (mainv == NULL) {
      fprintf(stderr, "%s:1:1: E3007 missing main under vow run\n",
              display);
      return 3;
    }
    {
      Val *r = ev_call(&ev, mainv, NULL, 0, 1, 1);
      if (r == NULL) {
        fprintf(stderr, "%s:%d:%d: %s %s\n", ev.err_file,
                ev.err_line, ev.err_col, ev.err_code, ev.err_msg);
        return emit_class_exit(ev.err_code);
      }
      if (r->kind != V_INT) {
        fprintf(stderr, "%s:1:1: E5011 main must return an integer\n",
                display);
        return 5;
      }
      return (int)(r->ival & 0xFF);
    }
  }
}

/* Whole-file read in binary mode, capped. Mirrors the checker's
   read_capped contract with toolchain-side messages. */
static unsigned char g_emit_src[EMIT_FILE_CAP];

static int emit_read(const char *path, size_t *len) {
  FILE *f;
  size_t n = 0;
  size_t got;
  f = fopen(path, "rb");
  if (f == NULL) {
    return -1;
  }
  for (;;) {
    if (n == sizeof(g_emit_src)) {
      fclose(f);
      return -2;
    }
    got = fread(g_emit_src + n, 1, sizeof(g_emit_src) - n, f);
    n += got;
    if (got == 0) {
      break;
    }
  }
  fclose(f);
  *len = n;
  return 0;
}

static int emit_puts(FILE *f, const char *s) {
  while (*s != '\0') {
    if (fwrite(s, 1, 1, f) != 1) {
      return 0;
    }
    s += 1;
  }
  return 1;
}

/* One byte as backslash plus exactly three octal digits (never a
   greedy hex escape). */
static int emit_octal(FILE *f, unsigned char b) {
  char o[4];
  o[0] = (char)('0' + ((b >> 6) & 7));
  o[1] = (char)('0' + ((b >> 3) & 7));
  o[2] = (char)('0' + (b & 7));
  o[3] = '\0';
  if (fwrite("\\", 1, 1, f) != 1) {
    return 0;
  }
  return fwrite(o, 1, 3, f) == 3;
}

/* C-string literal body: printable bytes verbatim except '"' and
   '\\'; everything else octal. */
static int emit_cstr(FILE *f, const char *s) {
  size_t i = 0;
  while (s[i] != '\0') {
    unsigned char b = (unsigned char)s[i];
    if (b >= 32 && b < 127 && b != '"' && b != '\\') {
      if (fwrite(s + i, 1, 1, f) != 1) {
        return 0;
      }
    } else {
      if (!emit_octal(f, b)) {
        return 0;
      }
    }
    i += 1;
  }
  return 1;
}

static int emit_decimal(FILE *f, unsigned v) {
  char digits[4];
  size_t n = 0;
  if (v >= 100) {
    digits[n] = (char)('0' + v / 100);
    n += 1;
  }
  if (v >= 10) {
    digits[n] = (char)('0' + (v / 10) % 10);
    n += 1;
  }
  digits[n] = (char)('0' + v % 10);
  n += 1;
  digits[n] = '\0';
  return fwrite(digits, 1, n, f) == n;
}

/* Reference sources linked into every dylib (the whole
   interpreter; main.c excluded, the stub provides the entry). */
static const char *g_emit_sources[] = {
  "diag.c", "lex.c",   "ast.c",   "parse.c", "check.c",
  "sha256.c", "eval.c", "run.c",   "platform.c", "emit.c",
};
#define EMIT_NSRC 10

static int emit_probe(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  fclose(f);
  return 1;
}

static unsigned char g_emit_io[EMIT_IO_CAP];

int emit_build(const char *vowpath, const char *file,
               const char *out) {
  char base[EMIT_PATH_CAP];
  char stripped[EMIT_PATH_CAP];
  char sym[EMIT_PATH_CAP];
  char indir[EMIT_PATH_CAP];
  char outpath[EMIT_PATH_CAP];
  char outdir[EMIT_PATH_CAP];
  char stubpath[EMIT_PATH_CAP];
  char mappath[EMIT_PATH_CAP];
  char dynpath[EMIT_PATH_CAP];
  char rootabs[EMIT_PATH_CAP];
  char cwd[EMIT_PATH_CAP];
  char refdir[EMIT_PATH_CAP];
  size_t srclen = 0;
  size_t i = 0;
  /* 1. Basename, symbol, default output beside the input. */
  emit_basename(file, base, sizeof(base));
  if (base[0] == '\0') {
    diag_emit("vow", 1, 1, "E1001", "bad CLI arguments");
    return 1;
  }
  {
    size_t n = 0;
    while (base[n] != '\0') {
      n += 1;
    }
    if (emit_ends_vow(base)) {
      size_t m = n - 4;
      size_t k = 0;
      while (k < m && k + 1 < sizeof(stripped)) {
        stripped[k] = base[k];
        k += 1;
      }
      stripped[k] = '\0';
    } else {
      size_t k = 0;
      while (base[k] != '\0' && k + 1 < sizeof(stripped)) {
        stripped[k] = base[k];
        k += 1;
      }
      stripped[k] = '\0';
    }
  }
  if (!emit_mangle(stripped, sym, sizeof(sym))) {
    diag_emit(file, 1, 1, "E1001", "bad CLI arguments");
    return 1;
  }
  emit_dirname(file, indir, sizeof(indir));
  if (out != NULL) {
    i = 0;
    while (out[i] != '\0' && i + 1 < sizeof(outpath)) {
      outpath[i] = out[i];
      i += 1;
    }
    outpath[i] = '\0';
    if (out[i] != '\0') {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
  } else {
    if (!emit_join(outpath, sizeof(outpath), indir, stripped)) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    {
      /* Default suffix for the host OS (Linux here; .dylib and
         .dll are documented follow-ups in the README). */
      const char *suf = ".so";
      size_t o = 0;
      while (outpath[o] != '\0') {
        o += 1;
      }
      i = 0;
      while (suf[i] != '\0') {
        if (o + 1 >= sizeof(outpath)) {
          diag_emit(file, 1, 1, "E5006", "resource limit breach");
          return 5;
        }
        outpath[o] = suf[i];
        o += 1;
        i += 1;
      }
      outpath[o] = '\0';
    }
  }
  emit_dirname(outpath, outdir, sizeof(outdir));
  /* 2. Single read of the entry; the same bytes are checked below
     and embedded into the stub (no TOCTOU between check and use).
     Entry-missing (E1002) wins over output-directory problems:
     the entry is the primary operand. */
  {
    int rc = emit_read(file, &srclen);
    if (rc == -1) {
      diag_emit(file, 1, 1, "E1002", "entry file not found");
      return 1;
    }
    if (rc == -2) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
  }
  /* 3. Absolute source root: absolute inputs keep their directory
     (M1 absolute preservation); relative ones anchor at the build
     working directory so the dylib behaves like `vow run`
     regardless of the caller's directory. */
  if (indir[0] == '\0' && emit_is_abs(file)) {
    rootabs[0] = '/';
    rootabs[1] = '\0';
  } else if (emit_is_abs(indir)) {
    i = 0;
    while (indir[i] != '\0' && i + 1 < sizeof(rootabs)) {
      rootabs[i] = indir[i];
      i += 1;
    }
    rootabs[i] = '\0';
    if (indir[i] != '\0') {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
  } else {
    if (plat_cwd(cwd, sizeof(cwd)) != 0) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    if (emit_streq(indir, ".")) {
      i = 0;
      while (cwd[i] != '\0' && i + 1 < sizeof(rootabs)) {
        rootabs[i] = cwd[i];
        i += 1;
      }
      rootabs[i] = '\0';
    } else {
      if (!emit_join(rootabs, sizeof(rootabs), cwd, indir)) {
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
    }
  }
  /* The output directory must exist (same class as a missing test
     directory: E1003; an unreadable one is E5006). Checked after
     the entry read so E1002 keeps precedence, and before anything
     is written. */
  {
    int probe = plat_probe_dir(outdir);
    if (!probe) {
      if (plat_err() == PLAT_MISSING) {
        diag_emit(outpath, 1, 1, "E1003", "target directory missing");
        return 1;
      }
      diag_emit(outpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
  }
  /* Canonicalize the root in place: the evaluator prefix-matches
     the raw root against normalized resolutions, so an interior
     ".." would fence out its own tree (lock.check verifying
     false under an otherwise identical `vow run`). */
  {
    char canon[EMIT_PATH_CAP];
    size_t t = 0;
    if (!emit_normalize(rootabs, canon, sizeof(canon))) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    while (canon[t] != '\0' && t + 1 < sizeof(rootabs)) {
      rootabs[t] = canon[t];
      t += 1;
    }
    rootabs[t] = '\0';
  }
  /* 4. Check first (main required): on failure nothing is written
     and the check diagnostic plus class propagate. */
  {
    static unsigned char arena_buf[8 * 1024 * 1024];
    Arena arena;
    Checker chk;
    arena_init(&arena, arena_buf, sizeof(arena_buf));
    check_init(&chk, &arena);
    ev_memo_reset();
    if (!check_memory(&chk, file, rootabs, g_emit_src, srclen, 1)) {
      fprintf(stderr, "%s:%d:%d: %s %s\n", chk.err_file,
              chk.err_line, chk.err_col, chk.err_code,
              chk.err_msg);
      return emit_class_exit(chk.err_code);
    }
  }
  /* 5. Intermediates beside the output (auditable, deterministic
     names derived from the export symbol). */
  {
    char stubbase[EMIT_PATH_CAP];
    size_t t = 0;
    while (sym[t] != '\0' && t + 1 < sizeof(stubbase)) {
      stubbase[t] = sym[t];
      t += 1;
    }
    stubbase[t] = '\0';
    {
      const char *suf = ".abi.c";
      size_t o = t;
      i = 0;
      while (suf[i] != '\0') {
        if (o + 1 >= sizeof(stubbase)) {
          diag_emit(file, 1, 1, "E5006", "resource limit breach");
          return 5;
        }
        stubbase[o] = suf[i];
        o += 1;
        i += 1;
      }
      stubbase[o] = '\0';
    }
    if (!emit_join(stubpath, sizeof(stubpath), outdir, stubbase)) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    if (!emit_join(mappath, sizeof(mappath), outdir, "abi.map")) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    {
      /* Per-symbol version script beside the output (same
         collision class as the library itself). */
      char verbase[EMIT_PATH_CAP];
      size_t t = 0;
      while (sym[t] != '\0' && t + 1 < sizeof(verbase)) {
        verbase[t] = sym[t];
        t += 1;
      }
      verbase[t] = '\0';
      {
        const char *suf = ".ver";
        size_t o = t;
        i = 0;
        while (suf[i] != '\0') {
          if (o + 1 >= sizeof(verbase)) {
            diag_emit(file, 1, 1, "E5006", "resource limit breach");
            return 5;
          }
          verbase[o] = suf[i];
          o += 1;
          i += 1;
        }
        verbase[o] = '\0';
      }
      if (!emit_join(dynpath, sizeof(dynpath), outdir, verbase)) {
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
    }
  }
  /* 6. Stub generation. */
  {
    FILE *f = fopen(stubpath, "wb");
    size_t k = 0;
    size_t col = 0;
    if (f == NULL) {
      diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    if (!emit_puts(f, "/* Generated by `vow build --emit-dylib ") ||
        !emit_cstr(f, file) ||
        !emit_puts(f, "` - do not edit. */\n") ||
        !emit_puts(f, "#include <stddef.h>\n") ||
        !emit_puts(f, "int emit_run_embedded(const unsigned char *src, "
                      "size_t len,\n"
                      "                      const char *display, const "
                      "char *root);\n") ||
        !emit_puts(f, "static const unsigned char vow_src[] = {\n")) {
      fclose(f);
      diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    while (k < srclen) {
      if (col == 0 && !emit_puts(f, "  ")) {
        fclose(f);
        diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      if (!emit_decimal(f, g_emit_src[k]) ||
          !emit_puts(f, ",")) {
        fclose(f);
        diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      k += 1;
      col += 1;
      if (col == 12 || k == srclen) {
        if (!emit_puts(f, "\n")) {
          fclose(f);
          diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
          return 5;
        }
        col = 0;
      } else if (!emit_puts(f, " ")) {
        fclose(f);
        diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
    }
    if (!emit_puts(f, "};\n") ||
        !emit_puts(f, "static const char vow_root[] = \"") ||
        !emit_cstr(f, rootabs) ||
        !emit_puts(f, "\";\n") ||
        !emit_puts(f, "static const char vow_display[] = \"") ||
        !emit_cstr(f, file) ||
        !emit_puts(f, "\";\nint ") ||
        !emit_puts(f, sym) ||
        !emit_puts(f, "(void) {\n"
                      "  return emit_run_embedded(vow_src, "
                      "sizeof(vow_src), vow_display,\n"
                      "                           vow_root);\n"
                      "}\n")) {
      fclose(f);
      diag_emit(stubpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    fclose(f);
  }
  /* 7. abi.map (one sorted name, the spec artifact) and the
     linker version script derived from it (the hiding
     mechanism: listed symbol global, everything else local). */
  {
    FILE *f = fopen(mappath, "wb");
    if (f == NULL) {
      diag_emit(mappath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    if (!emit_puts(f, sym) || !emit_puts(f, "\n")) {
      fclose(f);
      diag_emit(mappath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    fclose(f);
  }
  {
    FILE *f = fopen(dynpath, "wb");
    if (f == NULL) {
      diag_emit(dynpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    if (!emit_puts(f, "{\n global:\n  ") || !emit_puts(f, sym) ||
        !emit_puts(f, ";\n local:\n  *;\n};\n")) {
      fclose(f);
      diag_emit(dynpath, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    fclose(f);
  }
  /* 8. Reference sources: beside the binary (manual build), two
     levels up (zig-out layout), or under the working directory
     (repository-root invocation). First hit wins, deterministic
     order. Absent sources are E1003: the tool cannot find what it
     needs, same class as a missing test directory. */
  {
    char vowdir[EMIT_PATH_CAP];
    char cand[EMIT_PATH_CAP];
    char up2[EMIT_PATH_CAP];
    char cwdref[EMIT_PATH_CAP];
    int found = 0;
    emit_dirname(vowpath, vowdir, sizeof(vowdir));
    if (emit_join(cand, sizeof(cand), vowdir, "check.c") &&
        emit_probe(cand)) {
      size_t t = 0;
      while (vowdir[t] != '\0' && t + 1 < sizeof(refdir)) {
        refdir[t] = vowdir[t];
        t += 1;
      }
      refdir[t] = '\0';
      found = 1;
    }
    if (!found && emit_join(up2, sizeof(up2), vowdir, "../..") &&
        emit_join(cand, sizeof(cand), up2, "check.c") &&
        emit_probe(cand)) {
      size_t t = 0;
      while (up2[t] != '\0' && t + 1 < sizeof(refdir)) {
        refdir[t] = up2[t];
        t += 1;
      }
      refdir[t] = '\0';
      found = 1;
    }
    if (!found) {
      if (plat_cwd(cwd, sizeof(cwd)) != 0) {
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      if (emit_join(cwdref, sizeof(cwdref), cwd, "reference") &&
          emit_join(cand, sizeof(cand), cwdref, "check.c") &&
          emit_probe(cand)) {
        size_t t = 0;
        while (cwdref[t] != '\0' && t + 1 < sizeof(refdir)) {
          refdir[t] = cwdref[t];
          t += 1;
        }
        refdir[t] = '\0';
        found = 1;
      }
    }
    if (!found) {
      diag_emit(file, 1, 1, "E1003", "reference sources missing");
      return 1;
    }
  }
  /* 9. Link with the locked flags plus the shared-library surface
     (-fPIC -shared plus the dynamic-list pinning exports to
     abi.map). cc resolves through PATH like every spawned tool. */
  {
    char dynflag[EMIT_PATH_CAP];
    char srcpaths[EMIT_NSRC][EMIT_PATH_CAP];
    char *args[64];
    size_t na = 0;
    char out_tpl[EMIT_PATH_CAP];
    char err_tpl[EMIT_PATH_CAP];
    int out_fd;
    int err_fd;
    pid_t pid;
    int exit_code = 0;
    size_t s = 0;
    if (!emit_concat(dynflag, sizeof(dynflag),
                     "-Wl,--version-script=", dynpath)) {
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    args[na] = (char *)"cc";
    na += 1;
    {
      /* Locked flags verbatim, then the shared-library surface
         (-fPIC -shared plus a version script derived from
         abi.map, so the only dynamic export is the listed
         symbol; no source annotation is used anywhere). */
      static const char *flags[] = {
        "-std=c11", "-pedantic", "-Wall", "-Wextra", "-Werror",
        "-O2", "-fPIC", "-shared",
      };
      size_t fi = 0;
      while (fi < 8 && na + 1 < 60) {
        args[na] = (char *)flags[fi];
        na += 1;
        fi += 1;
      }
    }
    args[na] = stubpath;
    na += 1;
    while (s < EMIT_NSRC) {
      if (!emit_join(srcpaths[s], sizeof(srcpaths[s]), refdir,
                     g_emit_sources[s])) {
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      if (na + 1 >= 60) {
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      args[na] = srcpaths[s];
      na += 1;
      s += 1;
    }
    args[na] = (char *)"-o";
    na += 1;
    args[na] = outpath;
    na += 1;
    args[na] = dynflag;
    na += 1;
    args[na] = NULL;
    {
      const char *ot = "/tmp/vowccoutXXXXXX";
      const char *et = "/tmp/vowccerrXXXXXX";
      size_t t = 0;
      while (ot[t] != '\0' && t + 1 < sizeof(out_tpl)) {
        out_tpl[t] = ot[t];
        t += 1;
      }
      out_tpl[t] = '\0';
      t = 0;
      while (et[t] != '\0' && t + 1 < sizeof(err_tpl)) {
        err_tpl[t] = et[t];
        t += 1;
      }
      err_tpl[t] = '\0';
    }
    out_fd = plat_mktemp(out_tpl);
    err_fd = plat_mktemp(err_tpl);
    if (out_fd < 0 || err_fd < 0) {
      if (out_fd >= 0) {
        plat_close(out_fd);
      }
      if (err_fd >= 0) {
        plat_close(err_fd);
      }
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    pid = plat_spawn_redir(args[0], args, out_fd, err_fd);
    if (pid < 0) {
      plat_close(out_fd);
      plat_close(err_fd);
      plat_unlink(out_tpl);
      plat_unlink(err_tpl);
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    plat_close(out_fd);
    plat_close(err_fd);
    if (plat_wait(pid, &exit_code) != 0) {
      plat_unlink(out_tpl);
      plat_unlink(err_tpl);
      diag_emit(file, 1, 1, "E5006", "resource limit breach");
      return 5;
    }
    {
      /* Read both captures back for the failure path, then remove
         the temps either way. */
      FILE *fo = fopen(out_tpl, "rb");
      FILE *fe = fopen(err_tpl, "rb");
      size_t olen = 0;
      size_t elen = 0;
      size_t got;
      plat_unlink(out_tpl);
      plat_unlink(err_tpl);
      if (fo == NULL || fe == NULL) {
        if (fo != NULL) {
          fclose(fo);
        }
        if (fe != NULL) {
          fclose(fe);
        }
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
      for (;;) {
        if (olen == sizeof(g_emit_io)) {
          break;
        }
        got = fread(g_emit_io + olen, 1, sizeof(g_emit_io) - olen,
                    fo);
        olen += got;
        if (got == 0) {
          break;
        }
      }
      fclose(fo);
      for (;;) {
        if (olen + elen == sizeof(g_emit_io)) {
          break;
        }
        got = fread(g_emit_io + olen + elen, 1,
                    sizeof(g_emit_io) - olen - elen, fe);
        elen += got;
        if (got == 0) {
          break;
        }
      }
      fclose(fe);
      /* Exec-failure marker (same protocol as proc.spawn): the C
         compiler is not installed. Tool-level miss, exit 1. */
      if (exit_code == 127 && elen == 1 &&
          g_emit_io[olen] == '\0') {
        diag_emit(file, 1, 1, "E1003", "C compiler not found");
        return 1;
      }
      if (exit_code != 0) {
        if (olen + elen > 0) {
          fwrite(g_emit_io, 1, olen + elen, stderr);
        }
        diag_emit(file, 1, 1, "E5006", "resource limit breach");
        return 5;
      }
    }
  }
  return 0;
}
