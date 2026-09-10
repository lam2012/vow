#include "check.h"

#include <stdio.h>
#include <string.h>

#include "sha256.h"

/* Bounds. Exceeding one is E5006: the input, not the checker, is at
   fault, and every bound is documented here. */
#define CHECK_MAX_TOPS 4096
#define CHECK_MAX_SCOPE 256
#define CHECK_FILE_CAP ((size_t)64 * (size_t)1024 * (size_t)1024)
#define CHECK_MAX_DUP_SCAN 4096

/* Binding kinds for lexical resolution. */
enum { BK_PARAM, BK_LOCAL, BK_TOPFUNC, BK_TOPCONST, BK_IMPORT };

typedef struct {
  Slice name;
  int kind;
  Decl *decl;
  Module *mod;
} Binding;

typedef struct Scope Scope;
struct Scope {
  Binding items[CHECK_MAX_SCOPE];
  size_t n;
  Scope *parent;
};

/* Check context: the clause in force (null for test bodies, which
   check at the callee instead), and purity flags. Scopes are never
   stored here; they travel as explicit parameters. */
typedef struct {
  Checker *c;
  Module *mod;
  EffectEntry *clause;
  int in_test;
  int in_const;
} Ctx;

static int slice_eq(Slice a, const char *b) {
  size_t i = 0;
  while (b[i] != '\0') {
    i += 1;
  }
  if (a.n != i) {
    return 0;
  }
  for (i = 0; i < a.n; i++) {
    if (a.p[i] != (unsigned char)b[i]) {
      return 0;
    }
  }
  return 1;
}

static int slice_eq2(Slice a, Slice b) {
  size_t i;
  if (a.n != b.n) {
    return 0;
  }
  for (i = 0; i < a.n; i++) {
    if (a.p[i] != b.p[i]) {
      return 0;
    }
  }
  return 1;
}

static int dotted_eq(const Slice *a, size_t na, const Slice *b,
                     size_t nb) {
  size_t i;
  if (na != nb) {
    return 0;
  }
  for (i = 0; i < na; i++) {
    if (!slice_eq2(a[i], b[i])) {
      return 0;
    }
  }
  return 1;
}

Cap cap_lookup(const Slice *names, size_t n) {
  if (n != 2) {
    return CAP_NONE;
  }
  if (slice_eq(names[0], "fs") && slice_eq(names[1], "read")) {
    return CAP_FS_READ;
  }
  if (slice_eq(names[0], "fs") && slice_eq(names[1], "write")) {
    return CAP_FS_WRITE;
  }
  if (slice_eq(names[0], "fs") && slice_eq(names[1], "list")) {
    return CAP_FS_LIST;
  }
  if (slice_eq(names[0], "proc") && slice_eq(names[1], "spawn")) {
    return CAP_PROC_SPAWN;
  }
  if (slice_eq(names[0], "hash") && slice_eq(names[1], "sha256")) {
    return CAP_HASH_SHA256;
  }
  if (slice_eq(names[0], "lock") && slice_eq(names[1], "check")) {
    return CAP_LOCK_CHECK;
  }
  return CAP_NONE;
}

int cap_is_root(const Slice *names, size_t n) {
  if (n == 0) {
    return 0;
  }
  return slice_eq(names[0], "fs") || slice_eq(names[0], "proc") ||
         slice_eq(names[0], "hash") || slice_eq(names[0], "lock");
}

static void check_fail(Checker *c, const char *file, int line, int col,
                       const char *code, const char *msg) {
  size_t i = 0;
  if (c->failed) {
    return;
  }
  c->failed = 1;
  while (file[i] != '\0' && i + 1 < sizeof(c->err_file)) {
    c->err_file[i] = file[i];
    i += 1;
  }
  c->err_file[i] = '\0';
  c->err_line = line;
  c->err_col = col;
  c->err_code = code;
  c->err_msg = msg;
}

void check_init(Checker *c, Arena *arena) {
  c->arena = arena;
  c->root[0] = '.';
  c->root[1] = '\0';
  c->nmod = 0;
  c->nloading = 0;
  c->failed = 0;
  c->err_file[0] = '\0';
  c->err_line = 1;
  c->err_col = 1;
  c->err_code = "";
  c->err_msg = "";
}

/* Splits a path on '/' into segments. Returns the count, or -1 when
   the fixed buffer would overflow. */
#define CHECK_MAX_SEGS 128
#define CHECK_SEG_CAP 256

static int split_path(const char *path, char segs[][CHECK_SEG_CAP],
                      int *ns) {
  int n = 0;
  size_t i = 0;
  size_t k = 0;
  for (;;) {
    char ch = path[i];
    if (ch == '\0' || ch == '/') {
      if (k > 0) {
        if (n >= CHECK_MAX_SEGS) {
          return -1;
        }
        segs[n][k] = '\0';
        n += 1;
        k = 0;
      }
      if (ch == '\0') {
        break;
      }
      i += 1;
      continue;
    }
    if (k + 1 >= CHECK_SEG_CAP) {
      return -1;
    }
    segs[n][k] = ch;
    k += 1;
    i += 1;
  }
  *ns = n;
  return 0;
}

static int seg_is(const char *s, const char *t) {
  size_t i = 0;
  for (;;) {
    if (s[i] != t[i]) {
      return s[i] == '\0' && t[i] == '\0';
    }
    if (s[i] == '\0') {
      return 1;
    }
    i += 1;
  }
}

static void seg_copy(char *dst, const char *src) {
  size_t k = 0;
  while (src[k] != '\0') {
    dst[k] = src[k];
    k += 1;
  }
  dst[k] = '\0';
}

/* Pushes one segment with depth tracking. A ".." that would descend
   below base_depth is an escape; anything else applies normally.
   Returns 0 applied, 1 escape, -1 overflow. */
static int push_seg(char segs[][CHECK_SEG_CAP], int *n, int *depth,
                    int base_depth, const char *seg) {
  if (seg_is(seg, ".")) {
    return 0;
  }
  if (seg_is(seg, "..")) {
    if (*n > 0 && !seg_is(segs[*n - 1], "..")) {
      if (*depth - 1 < base_depth) {
        return 1;
      }
      *n -= 1;
      *depth -= 1;
      return 0;
    }
    return 1;
  }
  if (*n >= CHECK_MAX_SEGS) {
    return -1;
  }
  seg_copy(segs[*n], seg);
  *n += 1;
  *depth += 1;
  return 0;
}

/* Resolves an import to a display path anchored at the root. root
   is the entry file's directory ("." when bare, absolute when the
   entry path is absolute); impdir is the importing module's
   root-relative directory ("" for the root itself); rel is the raw
   import target. Absolute roots stay absolute in the output, so an
   absolute entry path keeps working; Windows drive letters are a
   documented follow-up, not handled here. Returns 0 with out set, 1
   on escape above the root, -1 on overflow, -2 on malformed input
   (empty target or backslash separator). */
static int join_norm(const char *root, const char *impdir,
                     const Slice *rel, char *out, size_t cap) {
  char segs[CHECK_MAX_SEGS][CHECK_SEG_CAP];
  char tmp[VOW_PATH_CAP];
  char rsegs[CHECK_MAX_SEGS][CHECK_SEG_CAP];
  int n = 0;
  int depth = 0;
  int base_depth;
  int absolute;
  int rn = 0;
  int r;
  size_t i;
  size_t pos = 0;
  size_t k;
  absolute = (root[0] == '/');
  if (rel->n == 0 || rel->n >= sizeof(tmp)) {
    return -2;
  }
  for (i = 0; i < rel->n; i++) {
    if (rel->p[i] == '\\' || rel->p[i] == '\0') {
      return -2;
    }
    tmp[i] = (char)rel->p[i];
  }
  tmp[rel->n] = '\0';
  /* Base: the normalized root. Leading ".." segments are kept and
     count below zero, so descents under the root's own depth are
     escapes even for roots above the working directory. */
  if (split_path(root, rsegs, &rn) != 0) {
    return -1;
  }
  for (r = 0; r < rn; r++) {
    if (seg_is(rsegs[r], ".")) {
      continue;
    }
    if (seg_is(rsegs[r], "..")) {
      if (n > 0 && !seg_is(segs[n - 1], "..")) {
        n -= 1;
        depth -= 1;
        continue;
      }
      if (n >= CHECK_MAX_SEGS) {
        return -1;
      }
      seg_copy(segs[n], "..");
      n += 1;
      depth -= 1;
      continue;
    }
    if (n >= CHECK_MAX_SEGS) {
      return -1;
    }
    seg_copy(segs[n], rsegs[r]);
    n += 1;
    depth += 1;
  }
  base_depth = depth;
  /* Importer directory, root-relative. */
  if (!(impdir[0] == '\0')) {
    rn = 0;
    if (split_path(impdir, rsegs, &rn) != 0) {
      return -1;
    }
    for (r = 0; r < rn; r++) {
      int rc = push_seg(segs, &n, &depth, base_depth, rsegs[r]);
      if (rc == 1) {
        return 1;
      }
      if (rc != 0) {
        return -1;
      }
    }
  }
  /* Import target. */
  rn = 0;
  if (split_path(tmp, rsegs, &rn) != 0) {
    return -1;
  }
  if (rn == 0) {
    return -2;
  }
  for (r = 0; r < rn; r++) {
    int rc = push_seg(segs, &n, &depth, base_depth, rsegs[r]);
    if (rc == 1) {
      return 1;
    }
    if (rc != 0) {
      return -1;
    }
  }
  if (absolute && n == 0) {
    if (2 > cap) {
      return -1;
    }
    out[0] = '/';
    out[1] = '\0';
    return 0;
  }
  for (i = 0; i < (size_t)n; i++) {
    k = 0;
    if (pos > 0 || (absolute && i == 0)) {
      if (pos + 1 >= cap) {
        return -1;
      }
      out[pos] = '/';
      pos += 1;
    }
    while (segs[i][k] != '\0') {
      if (pos + 1 >= cap) {
        return -1;
      }
      out[pos] = segs[i][k];
      pos += 1;
      k += 1;
    }
  }
  if (pos + 1 > cap) {
    return -1;
  }
  out[pos] = '\0';
  return 0;
}

/* Root-relative directory of a module display path ("" for a file
   directly under the root). Returns 0, or -1 on overflow. */
static int mod_impdir(const char *root, const char *modpath, char *out,
                      size_t cap) {
  size_t rlen = 0;
  size_t rest;
  size_t last;
  size_t i;
  while (root[rlen] != '\0') {
    rlen += 1;
  }
  if (rlen == 1 && root[0] == '.') {
    rest = 0;
  } else {
    size_t j;
    for (j = 0; j < rlen; j++) {
      if (modpath[j] != root[j]) {
        return -1;
      }
    }
    if (modpath[rlen] != '/') {
      return -1;
    }
    rest = rlen + 1;
  }
  last = rest;
  i = rest;
  while (modpath[i] != '\0') {
    if (modpath[i] == '/') {
      last = i;
    }
    i += 1;
  }
  if (last + 1 >= cap) {
    return -1;
  }
  for (i = rest; i < last; i++) {
    out[i - rest] = modpath[i];
  }
  out[last - rest] = '\0';
  return 0;
}


/* Reads a whole file in binary mode, capped. Returns 1 with *len set,
   or 0 with the error fields set. */
static int read_capped(const char *path, unsigned char *buf, size_t cap,
                       size_t *len, const char **code, const char **msg) {
  FILE *f;
  size_t n = 0;
  size_t got;
  f = fopen(path, "rb");
  if (f == NULL) {
    *code = "E3003";
    *msg = "cannot read import target";
    return 0;
  }
  for (;;) {
    if (n == cap) {
      fclose(f);
      *code = "E5006";
      *msg = "resource limit breach";
      return 0;
    }
    got = fread(buf + n, 1, cap - n, f);
    n += got;
    if (got == 0) {
      break;
    }
  }
  fclose(f);
  *len = n;
  return 1;
}

static int hex_val(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return (int)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int)(c - 'a') + 10;
  }
  return -1;
}

static Module *find_module(Checker *c, const char *rel) {
  size_t i;
  for (i = 0; i < c->nmod; i++) {
    const char *p = c->modules[i]->path;
    size_t j = 0;
    int same = 1;
    /* Exact byte comparison without library calls. */
    for (;;) {
      if (p[j] != rel[j]) {
        same = 0;
        break;
      }
      if (p[j] == '\0') {
        break;
      }
      j += 1;
    }
    if (same) {
      return c->modules[i];
    }
  }
  return NULL;
}

/* Forward declarations for the mutually recursive loader/checker. */
static Module *load_module(Checker *c, const char *dir, const Slice *rel,
                           const Slice *pin, const char *site_file,
                           int site_line, int site_col);
static int check_module(Checker *c, Module *m);

static int on_stack(Checker *c, const char *rel) {
  size_t i;
  size_t j;
  for (i = 0; i < c->nloading; i++) {
    const char *p = c->loading[i];
    int same = 1;
    j = 0;
    for (;;) {
      if (p[j] != rel[j]) {
        same = 0;
        break;
      }
      if (p[j] == '\0') {
        break;
      }
      j += 1;
    }
    if (same) {
      return 1;
    }
  }
  return 0;
}

static unsigned char g_mod_buf[64 * 1024 * 1024];

/* Retained source pool: every loaded module's bytes stay alive as
   long as any AST borrows from them, so all sources coexist. The
   scratch buffer above is only for reading; exact-sized copies live
   here. Exhaustion is E5006. */
static unsigned char g_src_pool[64 * 1024 * 1024];
static size_t g_src_used = 0;

static unsigned char *src_keep(const unsigned char *p, size_t n) {
  size_t aligned;
  unsigned char *out;
  size_t i;
  if (n == 0) {
    n = 1;
  }
  aligned = arena_align_up(n);
  if (aligned > sizeof(g_src_pool) ||
      g_src_used > sizeof(g_src_pool) - aligned) {
    return NULL;
  }
  out = g_src_pool + g_src_used;
  g_src_used += aligned;
  for (i = 0; i < n; i++) {
    out[i] = p[i];
  }
  return out;
}

static Module *load_module(Checker *c, const char *dir, const Slice *rel,
                           const Slice *pin, const char *site_file,
                           int site_line, int site_col) {
  char joined[VOW_PATH_CAP];
  int jr;
  size_t len;
  const char *code;
  const char *msg;
  char hex[65];
  size_t i;
  Decl *decls;
  Module *m;
  if (c->failed) {
    return NULL;
  }
  jr = join_norm(c->root, dir, rel, joined, sizeof(joined));
  if (jr != 0) {
    if (jr > 0) {
      check_fail(c, site_file, site_line, site_col, "E3006",
                 "import escapes source root");
    } else if (jr < -1) {
      check_fail(c, site_file, site_line, site_col, "E3003",
                 "malformed import path");
    } else {
      check_fail(c, site_file, site_line, site_col, "E5006",
                 "resource limit breach");
    }
    return NULL;
  }
  if (on_stack(c, joined)) {
    check_fail(c, site_file, site_line, site_col, "E3004",
               "cyclic import");
    return NULL;
  }
  m = find_module(c, joined);
  if (m != NULL) {
    return m;
  }
  if (!read_capped(joined, g_mod_buf, sizeof(g_mod_buf), &len, &code,
                   &msg)) {
    check_fail(c, site_file, site_line, site_col, code, msg);
    return NULL;
  }
  /* Pins are exactly 64 lowercase hex chars; anything else, or any
     mismatch, is E3003 before any code runs. */
  if (pin->n != 64) {
    check_fail(c, site_file, site_line, site_col, "E3003",
               "import hash mismatch");
    return NULL;
  }
  for (i = 0; i < 64; i++) {
    if (hex_val(pin->p[i]) < 0) {
      check_fail(c, site_file, site_line, site_col, "E3003",
                 "import hash mismatch");
      return NULL;
    }
  }
  sha256_hex(g_mod_buf, len, hex);
  for (i = 0; i < 64; i++) {
    if (hex[i] != (char)pin->p[i]) {
      check_fail(c, site_file, site_line, site_col, "E3003",
                 "import hash mismatch");
      return NULL;
    }
  }
  {
    unsigned char *kept = src_keep(g_mod_buf, len);
    Parser parser;
    if (kept == NULL) {
      check_fail(c, site_file, site_line, site_col, "E5006",
                 "resource limit breach");
      return NULL;
    }
    parse_init(&parser, c->arena, joined, kept, len);
    decls = parse_program(&parser);
    if (parser.failed) {
      /* A broken dependency reports its own diagnostic at its own
         location; the exit class follows the code. */
      check_fail(c, joined, parser.err_line, parser.err_col,
                 parser.err_code, parser.err_msg);
      return NULL;
    }
  }
  if (c->nmod >= CHECK_MAX_MODULES) {
    check_fail(c, site_file, site_line, site_col, "E5006",
               "resource limit breach");
    return NULL;
  }
  m = (Module *)arena_alloc(c->arena, sizeof(Module));
  i = 0;
  while (joined[i] != '\0' && i + 1 < sizeof(m->path)) {
    m->path[i] = joined[i];
    i += 1;
  }
  m->path[i] = '\0';
  m->decls = decls;
  m->done = 0;
  c->modules[c->nmod] = m;
  c->nmod += 1;
  return m;
}

/* Scope helpers. */

static Binding *scope_find(Scope *s, Slice name) {
  size_t i;
  for (; s != NULL; s = s->parent) {
    for (i = 0; i < s->n; i++) {
      if (slice_eq2(s->items[i].name, name)) {
        return &s->items[i];
      }
    }
  }
  return NULL;
}

/* Inserts into one scope only. Returns 1 on success, 0 on
   duplicate, -1 on overflow. Callers map 0 to E3009 and -1 to
   E5006. */
static int scope_put(Scope *s, Slice name, int kind, Decl *decl,
                     Module *mod) {
  size_t i;
  for (i = 0; i < s->n; i++) {
    if (slice_eq2(s->items[i].name, name)) {
      return 0;
    }
  }
  if (s->n >= CHECK_MAX_SCOPE) {
    return -1;
  }
  s->items[s->n].name = name;
  s->items[s->n].kind = kind;
  s->items[s->n].decl = decl;
  s->items[s->n].mod = mod;
  s->n += 1;
  return 1;
}

/* Reports a binding outcome: 1 keeps going, 0 stops with an error
   already recorded. */
static int scope_kept(Checker *c, const char *file, int line, int col,
                      int rc) {
  if (rc == 1) {
    return 1;
  }
  if (rc == 0) {
    check_fail(c, file, line, col, "E3009", "duplicate declaration");
  } else {
    check_fail(c, file, line, col, "E5006", "resource limit breach");
  }
  return 0;
}

static int clause_has(Ctx *ctx, const Slice *names, size_t n) {
  EffectEntry *e;
  for (e = ctx->clause; e != NULL; e = e->next) {
    if (dotted_eq(e->names, e->nname, names, n)) {
      return 1;
    }
  }
  return 0;
}

/* Callee effect-set cover for R10: every effect the callee declares
   must appear in the caller's clause. */
static int covers(EffectEntry *caller, EffectEntry *callee) {
  EffectEntry *w;
  for (w = callee; w != NULL; w = w->next) {
    EffectEntry *v;
    int found = 0;
    for (v = caller; v != NULL; v = v->next) {
      if (dotted_eq(v->names, v->nname, w->names, w->nname)) {
        found = 1;
        break;
      }
    }
    if (!found) {
      return 0;
    }
  }
  return 1;
}

static const char *display_of(Ctx *ctx) {
  return ctx->mod->path;
}

static void fail_at(Ctx *ctx, int line, int col, const char *code,
                    const char *msg) {
  check_fail(ctx->c, display_of(ctx), line, col, code, msg);
}

static int check_expr(Ctx *ctx, Scope *scope, Expr *e);
static int check_block(Ctx *ctx, Scope *parent, Stmt *body);

/* First call node inside the expression, or null. Used to report
   E3008 at the call site rather than at the declaration. */
static Expr *find_call(Expr *e) {
  Expr *it;
  Expr *found;
  if (e == NULL) {
    return NULL;
  }
  switch (e->kind) {
  case E_CALL:
    return e;
  case E_BINARY:
    found = find_call(e->a);
    return (found != NULL) ? found : find_call(e->b);
  case E_UNARY:
    return find_call(e->a);
  case E_INDEX:
    found = find_call(e->a);
    return (found != NULL) ? found : find_call(e->b);
  case E_LIST:
  case E_MAP:
    for (it = e->items; it != NULL; it = it->next) {
      found = find_call(it);
      if (found != NULL) {
        return found;
      }
    }
    return NULL;
  default:
    return NULL;
  }
}

/* Resolves one call callee. Returns 1, or 0 with an error set. */
static int check_call(Ctx *ctx, Scope *scope, Expr *e) {
  size_t i;
  /* Pure contexts admit no calls at all; the position reported is
     the call site. */
  if (ctx->in_const) {
    fail_at(ctx, e->line, e->col, "E3008",
            "call inside pure context");
    return 0;
  }
  /* Capability path: known member of a root, else E3001. */
  if (cap_is_root(e->names, e->nname)) {
    if (cap_lookup(e->names, e->nname) == CAP_NONE) {
      fail_at(ctx, e->line, e->col, "E3001", "unknown effect name");
      return 0;
    }
    if (!clause_has(ctx, e->names, e->nname)) {
      fail_at(ctx, e->line, e->col, "E3000",
              "effect call not in clause");
      return 0;
    }
    for (i = 0; i < e->nargs; i++) {
      if (!check_expr(ctx, scope, e->args[i])) {
        return 0;
      }
    }
    return 1;
  }
  /* Import member: must exist in the target module. Its kind does
     not matter here; calling a non-function fails at runtime
     (E5008), per the no-static-types rule. */
  if (e->nname > 1) {
    Binding *b = scope_find(scope, e->names[0]);
    if (b == NULL || b->kind != BK_IMPORT) {
      fail_at(ctx, e->line, e->col, "E3009", "unknown name");
      return 0;
    }
    {
      Decl *d;
      int found = 0;
      for (d = b->mod->decls; d != NULL; d = d->next) {
        if ((d->kind == D_FUNC || d->kind == D_CONST) &&
            slice_eq2(d->name, e->names[1])) {
          found = 1;
          break;
        }
      }
      if (!found || e->nname > 2) {
        fail_at(ctx, e->line, e->col, "E3009", "unknown name");
        return 0;
      }
      /* R10 through the module boundary: the callee's set must fit
         the caller's clause, except from test bodies (R2). */
      if (!ctx->in_test && d->kind == D_FUNC) {
        if (!covers(ctx->clause, d->effects)) {
          fail_at(ctx, e->line, e->col, "E3000",
                  "effect call not in clause");
          return 0;
        }
      }
    }
    for (i = 0; i < e->nargs; i++) {
      if (!check_expr(ctx, scope, e->args[i])) {
        return 0;
      }
    }
    return 1;
  }
  /* Single name: lexical resolution, then R10 for known functions.
     Params, locals, and constants pass (runtime sorts out
     callability, E5008); unbound names are E3009. */
  {
    Binding *b = scope_find(scope, e->names[0]);
    if (b == NULL) {
      fail_at(ctx, e->line, e->col, "E3009", "unknown name");
      return 0;
    }
    if (b->kind == BK_TOPFUNC && !ctx->in_test) {
      if (!covers(ctx->clause, b->decl->effects)) {
        fail_at(ctx, e->line, e->col, "E3000",
                "effect call not in clause");
        return 0;
      }
    }
  }
  for (i = 0; i < e->nargs; i++) {
    if (!check_expr(ctx, scope, e->args[i])) {
      return 0;
    }
  }
  return 1;
}

static int check_expr(Ctx *ctx, Scope *scope, Expr *e) {
  Expr *it;
  size_t i;
  size_t j;
  if (e == NULL || ctx->c->failed) {
    return ctx->c->failed == 0;
  }
  switch (e->kind) {
  case E_CALL:
    return check_call(ctx, scope, e);
  case E_BINARY:
    return check_expr(ctx, scope, e->a) &&
           check_expr(ctx, scope, e->b);
  case E_UNARY:
    return check_expr(ctx, scope, e->a);
  case E_INDEX:
    return check_expr(ctx, scope, e->a) &&
           check_expr(ctx, scope, e->b);
  case E_IDENT: {
    Binding *b;
    if (e->nname > 1) {
      /* A bare dotted path is meaningless outside a call: module
         members and capabilities only act through calls. */
      fail_at(ctx, e->line, e->col, "E3009", "unknown name");
      return 0;
    }
    b = scope_find(scope, e->names[0]);
    if (b == NULL) {
      fail_at(ctx, e->line, e->col, "E3009", "unknown name");
      return 0;
    }
    if (b->kind == BK_IMPORT) {
      /* A bare import alias is not a value: members act only
         through calls. Without this, checking accepts what
         evaluation must reject (M2). */
      fail_at(ctx, e->line, e->col, "E3009", "unknown name");
      return 0;
    }
    return 1;
  }
  case E_LIST:
    for (it = e->items; it != NULL; it = it->next) {
      if (!check_expr(ctx, scope, it)) {
        return 0;
      }
    }
    return 1;
  case E_MAP:
    for (it = e->items; it != NULL; it = it->next) {
      if (!check_expr(ctx, scope, it)) {
        return 0;
      }
    }
    /* Duplicate keys are rejected at the second occurrence. */
    if (e->nitem > CHECK_MAX_DUP_SCAN) {
      fail_at(ctx, e->line, e->col, "E5006",
              "resource limit breach");
      return 0;
    }
    for (i = 0; i < e->nitem; i++) {
      for (j = 0; j < i; j++) {
        if (slice_eq2(e->keys[i], e->keys[j])) {
          fail_at(ctx, e->key_lines[i], e->key_cols[i], "E3005",
                  "duplicate map key");
          return 0;
        }
      }
    }
    return 1;
  default:
    return 1;
  }
}

/* Scopes travel as explicit parameters: every scope object below is a
   synchronous local, so no address ever outlives its frame. */
static int check_block(Ctx *ctx, Scope *parent, Stmt *body) {
  Scope inner;
  Stmt *s;
  inner.n = 0;
  inner.parent = parent;
  for (s = body; s != NULL; s = s->next) {
    if (ctx->c->failed) {
      return 0;
    }
    if (s->kind == S_LET) {
      int rc;
      if (cap_is_root(&s->name, 1)) {
        fail_at(ctx, s->line, s->col, "E3009",
                "reserved capability root");
        return 0;
      }
      /* The initializer is checked before binding, so a name never
         sees its own declaration. */
      if (!check_expr(ctx, &inner, s->e)) {
        return 0;
      }
      rc = scope_put(&inner, s->name, BK_LOCAL, NULL, NULL);
      if (rc == 0) {
        fail_at(ctx, s->line, s->col, "E3009",
                "duplicate declaration");
        return 0;
      }
      if (rc < 0) {
        fail_at(ctx, s->line, s->col, "E5006",
                "resource limit breach");
        return 0;
      }
    } else if (s->kind == S_ASSERT) {
      if (!check_expr(ctx, &inner, s->e)) {
        return 0;
      }
      if (s->msg != NULL && !check_expr(ctx, &inner, s->msg)) {
        return 0;
      }
    } else if (s->kind == S_EXPR) {
      if (!check_expr(ctx, &inner, s->e)) {
        return 0;
      }
    } else if (s->kind == S_IF) {
      if (!check_expr(ctx, &inner, s->e)) {
        return 0;
      }
      if (!check_block(ctx, &inner, s->body)) {
        return 0;
      }
      if (s->els != NULL && !check_block(ctx, &inner, s->els)) {
        return 0;
      }
    } else if (s->kind == S_FOR) {
      Scope loop;
      if (!check_expr(ctx, &inner, s->e)) {
        return 0;
      }
      if (cap_is_root(&s->name, 1)) {
        fail_at(ctx, s->line, s->col, "E3009",
                "reserved capability root");
        return 0;
      }
      loop.n = 0;
      loop.parent = &inner;
      if (scope_put(&loop, s->name, BK_LOCAL, NULL, NULL) <= 0) {
        fail_at(ctx, s->line, s->col, "E3009",
                "duplicate declaration");
        return 0;
      }
      if (!check_block(ctx, &loop, s->body)) {
        return 0;
      }
    } else if (s->kind == S_RETURN) {
      if (s->e != NULL && !check_expr(ctx, &inner, s->e)) {
        return 0;
      }
    }
  }
  return ctx->c->failed == 0;
}

/* Loads every module this module imports (verification included).
   Binding work happens once in bind_tops below. */
static int load_imports(Checker *c, Module *m) {
  Decl *d;
  char impdir[VOW_PATH_CAP];
  for (d = m->decls; d != NULL; d = d->next) {
    Module *target;
    if (d->kind != D_IMPORT) {
      continue;
    }
    if (mod_impdir(c->root, m->path, impdir, sizeof(impdir)) != 0) {
      check_fail(c, m->path, d->line, d->col, "E5006",
                 "resource limit breach");
      return 0;
    }
    target = load_module(c, impdir, &d->path, &d->hash, m->path,
                         d->line, d->col);
    if (c->failed) {
      (void)target;
      return 0;
    }
  }
  return c->failed == 0;
}

/* Builds one module's top-level scope: import aliases bound to their
   loaded targets, functions and constants bound to their
   declarations. Reserved capability roots and duplicates are E3009.
   This is the single place top-level bindings are made. */
static int bind_tops(Checker *c, Module *m, Scope *tops) {
  Decl *d;
  size_t count = 0;
  tops->n = 0;
  tops->parent = NULL;
  for (d = m->decls; d != NULL; d = d->next) {
    count += 1;
    if (count > CHECK_MAX_TOPS) {
      check_fail(c, m->path, d->line, d->col, "E5006",
                 "resource limit breach");
      return 0;
    }
    if (d->kind == D_IMPORT) {
      int rc;
      Module *target;
      char impdir[VOW_PATH_CAP];
      char joined[VOW_PATH_CAP];
      if (cap_is_root(&d->name, 1)) {
        check_fail(c, m->path, d->line, d->col, "E3009",
                   "reserved capability root");
        return 0;
      }
      if (mod_impdir(c->root, m->path, impdir, sizeof(impdir)) != 0) {
        check_fail(c, m->path, d->line, d->col, "E5006",
                   "resource limit breach");
        return 0;
      }
      {
        int jr = join_norm(c->root, impdir, &d->path, joined,
                           sizeof(joined));
        if (jr > 0) {
          check_fail(c, m->path, d->line, d->col, "E3006",
                     "import escapes source root");
          return 0;
        }
        if (jr < 0) {
          check_fail(c, m->path, d->line, d->col,
                     jr < -1 ? "E3003" : "E5006",
                     jr < -1 ? "malformed import path"
                             : "resource limit breach");
          return 0;
        }
      }
      target = find_module(c, joined);
      if (target == NULL) {
        check_fail(c, m->path, d->line, d->col, "E3003",
                   "cannot read import target");
        return 0;
      }
      /* Recorded for the evaluator: alias resolution is established
         once here, never re-derived. */
      d->target = target;
      rc = scope_put(tops, d->name, BK_IMPORT, d, target);
      if (!scope_kept(c, m->path, d->line, d->col, rc)) {
        return 0;
      }
    } else if (d->kind == D_FUNC || d->kind == D_CONST) {
      int rc;
      int kind = (d->kind == D_FUNC) ? BK_TOPFUNC : BK_TOPCONST;
      if (cap_is_root(&d->name, 1)) {
        check_fail(c, m->path, d->line, d->col, "E3009",
                   "reserved capability root");
        return 0;
      }
      rc = scope_put(tops, d->name, kind, d, m);
      if (!scope_kept(c, m->path, d->line, d->col, rc)) {
        return 0;
      }
    }
  }
  return c->failed == 0;
}

static int check_module(Checker *c, Module *m);

static int check_decl(Ctx *ctx, Scope *tops, Decl *d);

static int check_module(Checker *c, Module *m) {
  Decl *d;
  Ctx ctx;
  Scope tops;
  if (m->done) {
    return 1;
  }
  if (c->nloading >= CHECK_LOAD_DEPTH) {
    check_fail(c, m->path, 1, 1, "E5006", "resource limit breach");
    return 0;
  }
  c->loading[c->nloading] = m->path;
  c->nloading += 1;
  if (!load_imports(c, m)) {
    return 0;
  }
  ctx.c = c;
  ctx.mod = m;
  ctx.clause = NULL;
  ctx.in_test = 0;
  ctx.in_const = 0;
  if (!bind_tops(c, m, &tops)) {
    return 0;
  }
  for (d = m->decls; d != NULL; d = d->next) {
    if (!check_decl(&ctx, &tops, d)) {
      return 0;
    }
  }
  /* Check imported modules depth-first so every reachable body is
     verified; cycles were already excluded by the loading stack. */
  for (d = m->decls; d != NULL; d = d->next) {
    if (d->kind != D_IMPORT) {
      continue;
    }
    {
      Binding *b = scope_find(&tops, d->name);
      if (b != NULL && b->mod != NULL) {
        if (!check_module(c, b->mod)) {
          return 0;
        }
      }
    }
  }
  m->done = 1;
  c->nloading -= 1;
  return c->failed == 0;
}

static int check_effect_entries(Ctx *ctx, EffectEntry *list) {
  EffectEntry *e;
  for (e = list; e != NULL; e = e->next) {
    if (cap_lookup(e->names, e->nname) == CAP_NONE) {
      fail_at(ctx, e->line, e->col, "E3001", "unknown effect name");
      return 0;
    }
  }
  return 1;
}

static int check_decl(Ctx *ctx, Scope *tops, Decl *d) {
  size_t i;
  if (ctx->c->failed) {
    return 0;
  }
  switch (d->kind) {
  case D_IMPORT:
  case D_EFFECT:
    return 1;
  case D_CONST: {
    Ctx inner = *ctx;
    Expr *call;
    Scope noself;
    Binding *b;
    inner.in_const = 1;
    inner.in_test = 0;
    inner.clause = NULL;
    /* The initializer cannot see its own binding: self-reference is
       E3009, decided here rather than as a runtime cycle. */
    noself.n = 0;
    noself.parent = tops;
    for (b = tops->items; (size_t)(b - tops->items) < tops->n; b++) {
      if (b->decl != d) {
        int rc = scope_put(&noself, b->name,
                           b->kind == BK_IMPORT ? BK_IMPORT
                           : (b->kind == BK_TOPFUNC ? BK_TOPFUNC
                                                   : BK_TOPCONST),
                           b->decl, b->mod);
        if (rc <= 0) {
          fail_at(ctx, d->line, d->col,
                  rc == 0 ? "E3009" : "E5006",
                  rc == 0 ? "duplicate declaration"
                          : "resource limit breach");
          return 0;
        }
      }
    }
    call = find_call(d->value);
    if (call != NULL) {
      fail_at(ctx, call->line, call->col, "E3008",
              "call inside pure context");
      return 0;
    }
    return check_expr(&inner, &noself, d->value);
  }
  case D_FUNC: {
    Ctx inner = *ctx;
    Scope params;
    params.n = 0;
    params.parent = tops;
    for (i = 0; i < d->nparam; i++) {
      if (cap_is_root(&d->params[i], 1)) {
        fail_at(ctx, d->line, d->col, "E3009",
                "reserved capability root");
        return 0;
      }
      {
        int rc = scope_put(&params, d->params[i], BK_PARAM, d,
                           ctx->mod);
        if (!scope_kept(ctx->c, ctx->mod->path, d->line, d->col, rc)) {
          return 0;
        }
      }
    }
    if (!check_effect_entries(ctx, d->effects)) {
      return 0;
    }
    inner.clause = d->effects;
    inner.in_test = 0;
    inner.in_const = 0;
    return check_block(&inner, &params, d->body);
  }
  case D_TEST: {
    Ctx inner = *ctx;
    inner.clause = NULL;
    inner.in_test = 1;
    inner.in_const = 0;
    return check_block(&inner, tops, d->body);
  }
  }
  return 1;
}

/* Shared entry tail: parse already-retained bytes under display
   and root, register the entry module, and check it. check_file
   (disk entry) and check_memory (embedded entry for the ABI stub)
   differ only in where the bytes and the root come from; behavior
   below this point is identical by construction. */
static int check_entry_bytes(Checker *c, const char *display,
                             const char *root, unsigned char *kept,
                             size_t len, int require_main) {
  Decl *decls;
  Module *entry;
  {
    size_t i = 0;
    while (root[i] != '\0' && i + 1 < sizeof(c->root)) {
      c->root[i] = root[i];
      i += 1;
    }
    c->root[i] = '\0';
  }
  {
    Parser parser;
    parse_init(&parser, c->arena, display, kept, len);
    decls = parse_program(&parser);
    if (parser.failed) {
      check_fail(c, display, parser.err_line, parser.err_col,
                 parser.err_code, parser.err_msg);
      return 0;
    }
  }
  if (c->nmod + 1 >= CHECK_MAX_MODULES) {
    check_fail(c, display, 1, 1, "E5006", "resource limit breach");
    return 0;
  }
  entry = (Module *)arena_alloc(c->arena, sizeof(Module));
  {
    size_t i = 0;
    while (display[i] != '\0' && i + 1 < sizeof(entry->path)) {
      entry->path[i] = display[i];
      i += 1;
    }
    entry->path[i] = '\0';
  }
  entry->decls = decls;
  entry->done = 0;
  c->modules[c->nmod] = entry;
  c->nmod += 1;
  /* The entry is its own cycle guard: a module importing the entry
     path closes a loop. */
  if (c->nloading >= CHECK_LOAD_DEPTH) {
    check_fail(c, display, 1, 1, "E5006", "resource limit breach");
    return 0;
  }
  c->loading[c->nloading] = entry->path;
  c->nloading += 1;
  if (!check_module(c, entry)) {
    return 0;
  }
  if (require_main) {
    Decl *d;
    int found = 0;
    for (d = entry->decls; d != NULL; d = d->next) {
      if (d->kind == D_FUNC && slice_eq(d->name, "main") &&
          d->nparam == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      check_fail(c, display, 1, 1, "E3007",
                 "missing main under vow run");
      return 0;
    }
  }
  return c->failed == 0;
}

int check_memory(Checker *c, const char *display, const char *root,
                 const unsigned char *src, size_t len,
                 int require_main) {
  char droot[VOW_PATH_CAP];
  char ddisplay[VOW_PATH_CAP];
  size_t i = 0;
  size_t k = 0;
  /* Roots and display paths are bounded exactly like disk entries;
     overlong inputs fail loudly before any parsing. */
  while (root[i] != '\0') {
    if (i + 1 >= sizeof(droot)) {
      check_fail(c, display, 1, 1, "E5006", "resource limit breach");
      return 0;
    }
    droot[i] = root[i];
    i += 1;
  }
  droot[i] = '\0';
  while (display[k] != '\0') {
    if (k + 1 >= sizeof(ddisplay)) {
      check_fail(c, display, 1, 1, "E5006", "resource limit breach");
      return 0;
    }
    ddisplay[k] = display[k];
    k += 1;
  }
  ddisplay[k] = '\0';
  {
    unsigned char *kept = src_keep(src, len);
    if (kept == NULL) {
      check_fail(c, ddisplay, 1, 1, "E5006", "resource limit breach");
      return 0;
    }
    return check_entry_bytes(c, ddisplay, droot, kept, len,
                             require_main);
  }
}

int check_file(Checker *c, const char *path, int require_main) {
  char root[VOW_PATH_CAP];
  char display[VOW_PATH_CAP];
  size_t len = 0;
  size_t last = 0;
  size_t has = 0;
  size_t n;
  const char *code;
  const char *msg;
  /* The source root is the entry file's directory ("." when bare).
     Display keeps the path exactly as given on the command line. */
  while (path[len] != '\0') {
    if (path[len] == '/' || path[len] == '\\') {
      last = len;
      has = 1;
    }
    len += 1;
  }
  if (has == 0) {
    root[0] = '.';
    root[1] = '\0';
  } else {
    if (last + 1 >= sizeof(root)) {
      check_fail(c, path, 1, 1, "E5006", "resource limit breach");
      return 0;
    }
    for (n = 0; n < last; n++) {
      root[n] = path[n];
    }
    root[last] = '\0';
  }
  {
    size_t i = 0;
    while (path[i] != '\0' && i + 1 < sizeof(display)) {
      display[i] = path[i];
      i += 1;
    }
    display[i] = '\0';
  }
  if (!read_capped(path, g_mod_buf, sizeof(g_mod_buf), &len, &code,
                   &msg)) {
    /* read_capped only fails two ways: a missing file (usage error
       for an entry path) or an oversized one (resource error). */
    if (code[0] == 'E' && code[1] == '3') {
      check_fail(c, path, 1, 1, "E1002", "entry file not found");
    } else {
      check_fail(c, path, 1, 1, code, msg);
    }
    return 0;
  }
  {
    unsigned char *kept = src_keep(g_mod_buf, len);
    if (kept == NULL) {
      check_fail(c, path, 1, 1, "E5006", "resource limit breach");
      return 0;
    }
    return check_entry_bytes(c, display, root, kept, len,
                             require_main);
  }
}
