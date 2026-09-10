/* Platform seam lives in platform.h/platform.c; this file
   includes no POSIX headers directly (see docs/c-subset.md). */

#include "eval.h"

#include <stdio.h>
#include <string.h>

#include "platform.h"

#include "diag.h"
#include "lex.h"
#include "sha256.h"

/* Value pool: 512 MiB of bump-only static storage, matching the
   default live-memory limit. Nothing is freed; exhaustion aborts
   with E5006, which is always safe (never a wrong answer). The steps
   cap bounds how fast garbage can accumulate. */
static unsigned char g_val_pool[512u * 1024u * 1024u];
static size_t g_val_used = 0;

/* Scratch for file reads and spawn capture (64 MiB cap each way). */
static unsigned char g_io_buf[64 * 1024 * 1024];

static void ev_fail(Ev *ev, int line, int col, const char *code,
                    const char *msg) {
  size_t i = 0;
  if (ev->failed) {
    return;
  }
  ev->failed = 1;
  while (ev->mod->path[i] != '\0' && i + 1 < sizeof(ev->err_file)) {
    ev->err_file[i] = ev->mod->path[i];
    i += 1;
  }
  ev->err_file[i] = '\0';
  ev->err_line = line;
  ev->err_col = col;
  ev->err_code = code;
  ev->err_msg = msg;
}

void ev_init(Ev *ev, Checker *chk, Module *entry, const char *root) {
  ev->chk = chk;
  ev->mod = entry;
  ev->root = root;
  ev->clause = NULL;
  ev->in_test = 0;
  ev->steps = 0;
  ev->depth = 0;
  ev->failed = 0;
  ev->err_file[0] = '\0';
  ev->err_line = 1;
  ev->err_col = 1;
  ev->err_code = "";
  ev->err_msg = "";
  ev->assert_failed = 0;
  ev->assert_line = 1;
  ev->assert_col = 1;
  ev->has_msg = 0;
}

static void *valloc(Ev *ev, size_t n, int line, int col) {
  void *out;
  n = arena_align_up(n);
  if (n == 0 || n > sizeof(g_val_pool) ||
      g_val_used > sizeof(g_val_pool) - n) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  out = (void *)(g_val_pool + g_val_used);
  g_val_used += n;
  memset(out, 0, n);
  return out;
}

/* One evaluation step. Exceeding the budget is E5006. */
static int tick(Ev *ev, int line, int col) {
  ev->steps += 1;
  if (ev->steps > EV_MAX_STEPS) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return 0;
  }
  return 1;
}

static Val *vnew(Ev *ev, VKind kind, int line, int col) {
  Val *v = (Val *)valloc(ev, sizeof(Val), line, col);
  if (v == NULL) {
    return NULL;
  }
  v->kind = kind;
  return v;
}

static Val *vint(Ev *ev, int64_t x, int line, int col) {
  Val *v = vnew(ev, V_INT, line, col);
  if (v == NULL) {
    return NULL;
  }
  v->ival = x;
  return v;
}

static Val *vbool(Ev *ev, int x, int line, int col) {
  Val *v = vnew(ev, V_BOOL, line, col);
  if (v == NULL) {
    return NULL;
  }
  v->ival = x ? 1 : 0;
  return v;
}

static Val *vstr(Ev *ev, const unsigned char *p, size_t n, int line,
                 int col) {
  Val *v = vnew(ev, V_STR, line, col);
  unsigned char *copy;
  size_t i;
  if (v == NULL) {
    return NULL;
  }
  copy = (unsigned char *)valloc(ev, n > 0 ? n : 1, line, col);
  if (copy == NULL) {
    return NULL;
  }
  for (i = 0; i < n; i++) {
    copy[i] = p[i];
  }
  v->s.p = copy;
  v->s.n = n;
  return v;
}

/* Checked integer arithmetic (the helpers the subset doc defers to
   this slice). Each returns 1 with *out set, or 0 for overflow. */
static int add_i64(int64_t a, int64_t b, int64_t *out) {
  if ((b > 0 && a > (int64_t)9223372036854775807LL - b) ||
      (b < 0 && a < (int64_t)(-9223372036854775807LL - 1) - b)) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int sub_i64(int64_t a, int64_t b, int64_t *out) {
  if ((b > 0 && a < (int64_t)(-9223372036854775807LL - 1) + b) ||
      (b < 0 && a > (int64_t)9223372036854775807LL + b)) {
    return 0;
  }
  *out = a - b;
  return 1;
}

static int mul_i64(int64_t a, int64_t b, int64_t *out) {
  int64_t lo = (int64_t)(-9223372036854775807LL - 1);
  int64_t hi = (int64_t)9223372036854775807LL;
  uint64_t ua;
  uint64_t ub;
  uint64_t mag;
  int neg;
  if (a == 0 || b == 0) {
    *out = 0;
    return 1;
  }
  if ((a == lo && b == -1) || (b == lo && a == -1)) {
    return 0;
  }
  neg = ((a < 0) != (b < 0));
  ua = (uint64_t)(a < 0 ? -(a + 1) + 1 : a);
  ub = (uint64_t)(b < 0 ? -(b + 1) + 1 : b);
  /* Magnitude check without overflowing: ua > hi/ub means overflow,
     careful when ub divides evenly is unnecessary since any excess
     bit fails. */
  (void)neg;
  if (ub != 0 && ua > (uint64_t)hi / ub) {
    /* Positive overflow, or negative beyond lo: recompute the bound
       for the negative case exactly. */
    if (!neg) {
      return 0;
    }
    if (ub != 0 && ua > ((uint64_t)hi + 1u) / ub) {
      return 0;
    }
    if (ua == ((uint64_t)hi + 1u) / ub &&
        ((uint64_t)hi + 1u) % ub != 0) {
      return 0;
    }
  }
  mag = ua * ub;
  if (!neg) {
    if (mag > (uint64_t)hi) {
      return 0;
    }
    *out = (int64_t)mag;
  } else {
    if (mag > (uint64_t)hi + 1u) {
      return 0;
    }
    if (mag == (uint64_t)hi + 1u) {
      *out = lo;
    } else {
      *out = -(int64_t)mag;
    }
  }
  return 1;
}

/* Byte-wise string order, locale-free. Negative, zero, positive. */
static int cmp_str(Slice a, Slice b) {
  size_t i = 0;
  while (i < a.n && i < b.n) {
    if (a.p[i] != b.p[i]) {
      return (a.p[i] < b.p[i]) ? -1 : 1;
    }
    i += 1;
  }
  if (a.n == b.n) {
    return 0;
  }
  return (a.n < b.n) ? -1 : 1;
}

static int slice_eqv(Slice a, Slice b) {
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

/* Structural equality. Cross-type comparison is false, never an
   error: equality applies to all values. Maps compare
   order-insensitively. */
static int val_eq(Val *a, Val *b) {
  size_t i;
  size_t j;
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case V_INT:
  case V_BOOL:
    return a->ival == b->ival;
  case V_STR:
    return slice_eqv(a->s, b->s);
  case V_LIST:
    if (a->n != b->n) {
      return 0;
    }
    for (i = 0; i < a->n; i++) {
      if (!val_eq(a->items[i], b->items[i])) {
        return 0;
      }
    }
    return 1;
  case V_MAP:
    if (a->n != b->n) {
      return 0;
    }
    for (i = 0; i < a->n; i++) {
      int found = 0;
      for (j = 0; j < b->n; j++) {
        if (slice_eqv(a->keys[i], b->keys[j]) &&
            val_eq(a->vals[i], b->vals[j])) {
          found = 1;
          break;
        }
      }
      if (!found) {
        return 0;
      }
    }
    return 1;
  case V_FUNC:
    return a->func == b->func && a->home == b->home;
  }
  return 0;
}

static Env *env_put(Ev *ev, Env *parent, Slice name, Val *val, int line,
                    int col) {
  Env *e = (Env *)valloc(ev, sizeof(Env), line, col);
  if (e == NULL) {
    return NULL;
  }
  e->name = name;
  e->val = val;
  e->parent = parent;
  return e;
}

static Val *env_find(Env *env, Slice name) {
  for (; env != NULL; env = env->parent) {
    if (slice_eqv(env->name, name)) {
      return env->val;
    }
  }
  return NULL;
}

/* Forward declarations. */
static Val *eval_call_site(Ev *ev, Env *env, Expr *e);

/* Resolves a display path for an fs path: absolute stays absolute,
   relative anchors at the source root. Both go through the same
   lexical normalization the checker uses. */
static int resolve_fs(Ev *ev, Slice p, char *out, size_t cap) {
  /* Reuse the checker's normalization through a synthetic import
     shape would twist meanings; paths here need no escape concept
     of their own because every result is prefix-matched against
     declared scopes below. A leading ".." that climbs above the
     anchor simply fails to match. */
  size_t base = 0;
  size_t i = 0;
  size_t k;
  size_t nseg = 0;
  static char segs[128][256];
  /* Anchor: root, or nothing when p is absolute. */
  if (!(p.n > 0 && p.p[0] == '/')) {
    const char *r = ev->root;
    while (r[base] != '\0') {
      base += 1;
    }
  }
  {
    /* Feed root segments (unless absolute) then path segments
       through one collapsing pass. */
    const unsigned char *parts[2];
    size_t lens[2];
    size_t np = 0;
    if (!(p.n > 0 && p.p[0] == '/')) {
      /* root bytes */
      size_t rl = 0;
      while (ev->root[rl] != '\0') {
        rl += 1;
      }
      if (!(rl == 1 && ev->root[0] == '.')) {
        parts[np] = (const unsigned char *)ev->root;
        lens[np] = rl;
        np += 1;
      }
    }
    parts[np] = p.p;
    lens[np] = p.n;
    np += 1;
    for (k = 0; k < np; k++) {
      size_t s = 0;
      size_t start = 0;
      for (s = 0; s <= lens[k]; s++) {
        unsigned char ch = (s < lens[k]) ? parts[k][s] : '/';
        if (ch == '/') {
          size_t slen = s - start;
          if (slen == 0 || (slen == 1 && parts[k][start] == '.')) {
            /* skip */
          } else if (slen == 2 && parts[k][start] == '.' &&
                     parts[k][start + 1] == '.') {
            if (nseg > 0) {
              nseg -= 1;
            } else {
              /* Above the anchor: keep one marker so the result
                 can never prefix-match an inside scope. */
              if (nseg + 1 >= 128) {
                return 0;
              }
              segs[nseg][0] = '.';
              segs[nseg][1] = '.';
              segs[nseg][2] = '\0';
              nseg += 1;
            }
          } else {
            size_t t;
            if (nseg >= 128 || slen >= 256) {
              return 0;
            }
            for (t = 0; t < slen; t++) {
              segs[nseg][t] = (char)parts[k][start + t];
            }
            segs[nseg][slen] = '\0';
            nseg += 1;
          }
          start = s + 1;
        }
      }
    }
  }
  {
    size_t pos = 0;
    int absolute =
        (p.n > 0 && p.p[0] == '/') || ev->root[0] == '/';
    if (absolute) {
      if (pos + 1 >= cap) {
        return 0;
      }
      out[pos] = '/';
      pos += 1;
    }
    /* Nothing left after collapsing means the root itself: spell it
       "." (relative) so scope matching sees a real directory, never
       an empty string. Absolute roots always keep segments here. */
    if (nseg == 0 && !absolute) {
      if (pos + 2 > cap) {
        return 0;
      }
      out[pos] = '.';
      pos += 1;
    }
    for (i = 0; i < nseg; i++) {
      size_t t = 0;
      if (pos > (absolute ? (size_t)1 : (size_t)0)) {
        if (pos + 1 >= cap) {
          return 0;
        }
        out[pos] = '/';
        pos += 1;
      }
      while (segs[i][t] != '\0') {
        if (pos + 1 >= cap) {
          return 0;
        }
        out[pos] = segs[i][t];
        pos += 1;
        t += 1;
      }
    }
    if (pos + 1 > cap) {
      return 0;
    }
    out[pos] = '\0';
    return 1;
  }
}

/* True when the resolved actual path falls under the resolved
   declared directory: equal, or a proper child at a separator
   boundary. An empty directory matches nothing (and is never
   indexed backwards: the old code read dir[-1] here). A lone "."
   covers in-root relative paths; ".." markers escaping the root
   never match it. */
static int scope_covers(const char *dir, const char *actual) {
  size_t i = 0;
  if (dir[0] == '\0') {
    return 0;
  }
  if (dir[0] == '.' && dir[1] == '\0') {
    if (actual[0] == '/') {
      return 0;
    }
    if (actual[0] == '.' && actual[1] == '.' &&
        (actual[2] == '/' || actual[2] == '\0')) {
      return 0;
    }
    return 1;
  }
  while (dir[i] != '\0') {
    if (actual[i] != dir[i]) {
      return 0;
    }
    i += 1;
  }
  if (actual[i] == '\0') {
    return 1;
  }
  if (dir[i - 1] == '/') {
    return 1;
  }
  return actual[i] == '/';
}


/* Reads a whole file into the scratch buffer, capped. Returns 1 with
   *len set; 0 missing/unreadable, -1 over cap. */
static int io_read(const char *path, size_t *len) {
  FILE *f;
  size_t n = 0;
  size_t got;
  f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  for (;;) {
    if (n == sizeof(g_io_buf)) {
      fclose(f);
      return -1;
    }
    got = fread(g_io_buf + n, 1, sizeof(g_io_buf) - n, f);
    n += got;
    if (got == 0) {
      break;
    }
  }
  fclose(f);
  *len = n;
  return 1;
}

/* Argument helpers: capability arity and shapes are exact; anything
   else is E5012. */

static int need_arity(Ev *ev, size_t have, size_t want, int line,
                      int col) {
  if (have != want) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return 0;
  }
  return 1;
}

static int need_str(Ev *ev, Val *v, int line, int col) {
  if (v->kind != V_STR) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return 0;
  }
  return 1;
}

/* Paths and argv cross into C strings at the OS boundary, where NUL
   truncates silently. Rejecting NUL up front (E5012) keeps the
   failure loud and auditable. Content strings may still carry NUL;
   only path/argv positions are checked. */
static int need_no_nul(Ev *ev, Slice s, int line, int col) {
  size_t i;
  for (i = 0; i < s.n; i++) {
    if (s.p[i] == '\0') {
      ev_fail(ev, line, col, "E5012", "runtime type error");
      return 0;
    }
  }
  return 1;
}

/* Every capability takes exactly one argument, and that argument is
   a list (R1). Returns the list, or null with E5012 set. */
static Val *need_list_arg(Ev *ev, Val **args, size_t nargs, int line,
                          int col) {
  if (!need_arity(ev, nargs, 1, line, col)) {
    return NULL;
  }
  if (args[0]->kind != V_LIST) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return NULL;
  }
  return args[0];
}

/* Element access with exact-count check. */
static int need_elems(Ev *ev, Val *list, size_t want, int line,
                      int col) {
  if (list->n != want) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return 0;
  }
  return 1;
}

/* True when a dotted entry name equals "a.b". */
static int entry_is(const EffectEntry *e, const char *a, const char *b) {
  size_t alen = 0;
  size_t blen = 0;
  size_t i;
  while (a[alen] != '\0') {
    alen += 1;
  }
  while (b[blen] != '\0') {
    blen += 1;
  }
  if (e->nname != 2 || e->names[0].n != alen || e->names[1].n != blen) {
    return 0;
  }
  for (i = 0; i < alen; i++) {
    if (e->names[0].p[i] != (unsigned char)a[i]) {
      return 0;
    }
  }
  for (i = 0; i < blen; i++) {
    if (e->names[1].p[i] != (unsigned char)b[i]) {
      return 0;
    }
  }
  return 1;
}

/* Resolves a declared scope literal the same way actual paths
   resolve, so prefix comparison happens in one space. */
static int resolve_declared(Ev *ev, Slice s, char *out, size_t cap) {
  size_t i;
  if (s.n > 0 && s.p[0] == '/') {
    if (s.n + 1 > cap) {
      return 0;
    }
    for (i = 0; i < s.n; i++) {
      out[i] = (char)s.p[i];
    }
    out[i] = '\0';
    return 1;
  }
  return resolve_fs(ev, s, out, cap);
}

/* Clause cover for fs.read/write/list: the resolved actual path must
   fall under a resolved declared directory of the same capability. */
static int clause_covers_path(Ev *ev, const char *cap_a,
                              const char *cap_b, const char *actual) {
  EffectEntry *e;
  for (e = ev->clause; e != NULL; e = e->next) {
    size_t k;
    if (!entry_is(e, cap_a, cap_b)) {
      continue;
    }
    for (k = 0; k < e->nargs; k++) {
      char dir[VOW_PATH_CAP];
      if (!resolve_declared(ev, e->args[k], dir, sizeof(dir))) {
        continue;
      }
      if (scope_covers(dir, actual)) {
        return 1;
      }
    }
  }
  return 0;
}

/* Clause cover for proc.spawn: items[0] must equal a declared
   command; the remaining items must match the declared patterns
   positionally (a pattern ending in '*' matches any continuation),
   with extra actual arguments free. */
static int clause_covers_spawn(Ev *ev, Val *list) {
  EffectEntry *e;
  Slice cmd;
  size_t nargs = list->n;
  Val **args = list->items;
  if (nargs == 0 || args[0]->kind != V_STR) {
    return 0;
  }
  cmd = args[0]->s;
  for (e = ev->clause; e != NULL; e = e->next) {
    size_t k;
    int ok;
    if (!entry_is(e, "proc", "spawn") || e->nargs == 0) {
      continue;
    }
    if (e->args[0].n != cmd.n) {
      continue;
    }
    ok = 1;
    for (k = 0; k < cmd.n; k++) {
      if (e->args[0].p[k] != cmd.p[k]) {
        ok = 0;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    for (k = 1; k < e->nargs && k < nargs; k++) {
      Slice pat = e->args[k];
      Slice act;
      size_t t;
      if (args[k]->kind != V_STR) {
        ok = 0;
        break;
      }
      act = args[k]->s;
      if (pat.n > 0 && pat.p[pat.n - 1] == '*') {
        if (act.n < pat.n - 1) {
          ok = 0;
          break;
        }
        for (t = 0; t < pat.n - 1; t++) {
          if (act.p[t] != pat.p[t]) {
            ok = 0;
            break;
          }
        }
      } else if (act.n != pat.n) {
        ok = 0;
        break;
      } else {
        for (t = 0; t < pat.n; t++) {
          if (act.p[t] != pat.p[t]) {
            ok = 0;
            break;
          }
        }
      }
      if (!ok) {
        break;
      }
    }
    if (ok) {
      return 1;
    }
  }
  return 0;
}

static Val *builtin_fs_read(Ev *ev, Val **args, size_t nargs, int line,
                            int col) {
  char actual[VOW_PATH_CAP];
  Val *list;
  size_t len;
  int rc;
  list = need_list_arg(ev, args, nargs, line, col);
  if (list == NULL || !need_elems(ev, list, 1, line, col) ||
      !need_str(ev, list->items[0], line, col)) {
    return NULL;
  }
  if (!need_no_nul(ev, list->items[0]->s, line, col)) {
    return NULL;
  }
  if (!resolve_fs(ev, list->items[0]->s, actual, sizeof(actual))) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (!clause_covers_path(ev, "fs", "read", actual)) {
    ev_fail(ev, line, col, "E5004", "capability scope violation");
    return NULL;
  }
  rc = io_read(actual, &len);
  if (rc < 0) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (rc == 0) {
    ev_fail(ev, line, col, "E5010", "file not found at runtime");
    return NULL;
  }
  if (!lex_valid_utf8(g_io_buf, len)) {
    ev_fail(ev, line, col, "E5007", "non-UTF-8 process output");
    return NULL;
  }
  return vstr(ev, g_io_buf, len, line, col);
}

static Val *builtin_fs_write(Ev *ev, Val **args, size_t nargs, int line,
                             int col) {
  char actual[VOW_PATH_CAP];
  FILE *f;
  Val *list;
  size_t written;
  list = need_list_arg(ev, args, nargs, line, col);
  if (list == NULL || !need_elems(ev, list, 2, line, col) ||
      !need_str(ev, list->items[0], line, col) ||
      !need_str(ev, list->items[1], line, col)) {
    return NULL;
  }
  if (!need_no_nul(ev, list->items[0]->s, line, col)) {
    return NULL;
  }
  if (!resolve_fs(ev, list->items[0]->s, actual, sizeof(actual))) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (!clause_covers_path(ev, "fs", "write", actual)) {
    ev_fail(ev, line, col, "E5004", "capability scope violation");
    return NULL;
  }
  f = fopen(actual, "wb");
  if (f == NULL) {
    ev_fail(ev, line, col, "E5010", "file not found at runtime");
    return NULL;
  }
  written = fwrite(list->items[1]->s.p, 1, list->items[1]->s.n, f);
  if (written != list->items[1]->s.n || fclose(f) != 0) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  return vint(ev, (int64_t)written, line, col);
}

/* Byte-wise name order, locale-free: negative, zero, positive. */
static int cmp_names(Slice a, Slice b) {
  size_t k = 0;
  while (k < a.n && k < b.n) {
    if (a.p[k] != b.p[k]) {
      return (a.p[k] < b.p[k]) ? -1 : 1;
    }
    k += 1;
  }
  if (a.n == b.n) {
    return 0;
  }
  return (a.n < b.n) ? -1 : 1;
}

/* Insertion sort over an index array by name order. Small inputs
   only; qsort would need a function pointer, which the subset
   bans. */
static void sort_names(Slice *names, size_t *idx, size_t n) {
  size_t i;
  size_t j;
  for (i = 0; i < n; i++) {
    idx[i] = i;
  }
  for (i = 1; i < n; i++) {
    size_t cur = idx[i];
    j = i;
    while (j > 0 && cmp_names(names[idx[j - 1]], names[cur]) > 0) {
      idx[j] = idx[j - 1];
      j -= 1;
    }
    idx[j] = cur;
  }
}

static Val *builtin_fs_list(Ev *ev, Val **args, size_t nargs, int line,
                            int col) {
  char actual[VOW_PATH_CAP];
  PlatDir d;
  Val *out;
  Val **items = NULL;
  Slice *names = NULL;
  size_t *order = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t i;
  struct dirent *ent;
  Val *list;
  list = need_list_arg(ev, args, nargs, line, col);
  if (list == NULL || !need_elems(ev, list, 1, line, col) ||
      !need_str(ev, list->items[0], line, col)) {
    return NULL;
  }
  if (!need_no_nul(ev, list->items[0]->s, line, col)) {
    return NULL;
  }
  if (!resolve_fs(ev, list->items[0]->s, actual, sizeof(actual))) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (!clause_covers_path(ev, "fs", "list", actual)) {
    ev_fail(ev, line, col, "E5004", "capability scope violation");
    return NULL;
  }
  if (plat_opendir(actual, &d) != 0) {
    if (plat_err() == PLAT_MISSING) {
      ev_fail(ev, line, col, "E5010", "file not found at runtime");
    } else {
      ev_fail(ev, line, col, "E5006", "resource limit breach");
    }
    return NULL;
  }
  for (;;) {
    size_t len = 0;
    Slice nm;
    unsigned char *copy;
    size_t t;
    Val *one;
    ent = plat_readdir(&d);
    if (ent == NULL) {
      if (plat_err() != PLAT_OK) {
        plat_closedir(&d);
        ev_fail(ev, line, col, "E5006", "resource limit breach");
        return NULL;
      }
      break;
    }
    if (!tick(ev, line, col)) {
      plat_closedir(&d);
      return NULL;
    }
    while (ent->d_name[len] != '\0') {
      len += 1;
    }
    if ((len == 1 && ent->d_name[0] == '.') ||
        (len == 2 && ent->d_name[0] == '.' && ent->d_name[1] == '.')) {
      continue;
    }
    if (n == cap) {
      size_t ncap = (cap == 0) ? 16 : cap * 2u;
      Val **nitems;
      Slice *nnames;
      size_t *norder;
      if (ncap <= cap) {
        plat_closedir(&d);
        ev_fail(ev, line, col, "E5006", "resource limit breach");
        return NULL;
      }
      nitems = (Val **)valloc(ev, ncap * sizeof(Val *), line, col);
      nnames = (Slice *)valloc(ev, ncap * sizeof(Slice), line, col);
      norder = (size_t *)valloc(ev, ncap * sizeof(size_t), line, col);
      if (nitems == NULL || nnames == NULL || norder == NULL) {
        plat_closedir(&d);
        return NULL;
      }
      for (t = 0; t < n; t++) {
        nitems[t] = items[t];
        nnames[t] = names[t];
      }
      items = nitems;
      names = nnames;
      order = norder;
      cap = ncap;
    }
    copy = (unsigned char *)valloc(ev, len > 0 ? len : 1, line, col);
    if (copy == NULL) {
      plat_closedir(&d);
      return NULL;
    }
    for (t = 0; t < len; t++) {
      copy[t] = (unsigned char)ent->d_name[t];
    }
    if (!lex_valid_utf8(copy, len)) {
      plat_closedir(&d);
      ev_fail(ev, line, col, "E5007", "non-UTF-8 process output");
      return NULL;
    }
    nm.p = copy;
    nm.n = len;
    one = vstr(ev, copy, len, line, col);
    if (one == NULL) {
      plat_closedir(&d);
      return NULL;
    }
    names[n] = nm;
    items[n] = one;
    n += 1;
  }
  plat_closedir(&d);
  sort_names(names, order, n);
  out = vnew(ev, V_LIST, line, col);
  if (out == NULL) {
    return NULL;
  }
  if (n > 0) {
    Val **sorted =
        (Val **)valloc(ev, n * sizeof(Val *), line, col);
    if (sorted == NULL) {
      return NULL;
    }
    for (i = 0; i < n; i++) {
      sorted[i] = items[order[i]];
    }
    out->items = sorted;
  } else {
    out->items = NULL;
  }
  out->n = n;
  return out;
}

/* Spawns a declared command with captured stdout/stderr through
   unlinked temp files (never pipes, so no producer/consumer deadlock
   is possible). The environment and working directory are inherited
   from the harness: R3-interim behavior until declaration syntax
   lands. An exec failure of a declared command is E5010. */
static Val *builtin_spawn(Ev *ev, Val **args, size_t nargs, int line,
                          int col) {
  char out_tpl[] = "/tmp/vow-out-XXXXXX";
  char err_tpl[] = "/tmp/vow-err-XXXXXX";
  char **argv = NULL;
  int out_fd = -1;
  int err_fd = -1;
  pid_t pid;
  int exit_code = 0;
  size_t i;
  size_t olen = 0;
  size_t elen = 0;
  Val *outv;
  Val *errv;
  Val *map;
  Slice keys[3];
  Val *vals[3];
  static const char k_exit[] = "exit";
  static const char k_out[] = "stdout";
  static const char k_err[] = "stderr";
  Val *list;
  if (nargs == 0) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return NULL;
  }
  for (i = 0; i < nargs; i++) {
    if (args[i]->kind != V_LIST) {
      ev_fail(ev, line, col, "E5012", "runtime type error");
      return NULL;
    }
  }
  /* Spawn takes its single list apart: element zero is the command,
     the rest are argv. */
  list = args[0];
  if (!clause_covers_spawn(ev, list)) {
    ev_fail(ev, line, col, "E5005", "undeclared spawn");
    return NULL;
  }
  for (i = 0; i < list->n; i++) {
    if (!need_str(ev, list->items[i], line, col) ||
        !need_no_nul(ev, list->items[i]->s, line, col)) {
      return NULL;
    }
  }
  argv = (char **)valloc(ev, (list->n + 1) * sizeof(char *), line,
                         col);
  if (argv == NULL) {
    return NULL;
  }
  for (i = 0; i < list->n; i++) {
    char *one =
        (char *)valloc(ev, list->items[i]->s.n + 1, line, col);
    size_t t;
    if (one == NULL) {
      return NULL;
    }
    for (t = 0; t < list->items[i]->s.n; t++) {
      one[t] = (char)list->items[i]->s.p[t];
    }
    one[list->items[i]->s.n] = '\0';
    argv[i] = one;
  }
  argv[list->n] = NULL;
  out_fd = plat_mktemp(out_tpl);
  err_fd = plat_mktemp(err_tpl);
  if (out_fd < 0 || err_fd < 0) {
    if (out_fd >= 0) {
      plat_close(out_fd);
    }
    if (err_fd >= 0) {
      plat_close(err_fd);
    }
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  /* Temp files stay linked until the parent has read them back;
     unique mkstemp names make this race-free. */
  pid = plat_spawn_redir(argv[0], argv, out_fd, err_fd);
  if (pid < 0) {
    plat_close(out_fd);
    plat_close(err_fd);
    plat_unlink(out_tpl);
    plat_unlink(err_tpl);
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  plat_close(out_fd);
  plat_close(err_fd);
  if (plat_wait(pid, &exit_code) != 0) {
    plat_unlink(out_tpl);
    plat_unlink(err_tpl);
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  {
    /* Read both captures back, capped; then remove the temps. */
    FILE *fo;
    FILE *fe;
    size_t got;
    fo = fopen(out_tpl, "rb");
    fe = fopen(err_tpl, "rb");
    plat_unlink(out_tpl);
    plat_unlink(err_tpl);
    if (fo == NULL || fe == NULL) {
      if (fo != NULL) {
        fclose(fo);
      }
      if (fe != NULL) {
        fclose(fe);
      }
      ev_fail(ev, line, col, "E5006", "resource limit breach");
      return NULL;
    }
    olen = 0;
    for (;;) {
      if (olen == sizeof(g_io_buf)) {
        fclose(fo);
        fclose(fe);
        ev_fail(ev, line, col, "E5006", "resource limit breach");
        return NULL;
      }
      got = fread(g_io_buf + olen, 1, sizeof(g_io_buf) - olen, fo);
      olen += got;
      if (got == 0) {
        break;
      }
    }
    fclose(fo);
    {
      /* Stash stdout length, then read stderr after it in the
         same buffer. */
      size_t olen_saved = olen;
      elen = 0;
      for (;;) {
        if (olen_saved + elen == sizeof(g_io_buf)) {
          fclose(fe);
          ev_fail(ev, line, col, "E5006", "resource limit breach");
          return NULL;
        }
        got = fread(g_io_buf + olen_saved + elen, 1,
                    sizeof(g_io_buf) - olen_saved - elen, fe);
        elen += got;
        if (got == 0) {
          break;
        }
      }
      fclose(fe);
      olen = olen_saved;
    }
  }
  {
    unsigned char *ob;
    unsigned char *eb;
    size_t t;
    /* Exit semantics (normal status, signaled as 128 plus signal)
       live in the seam; the marker check below is Vow policy. */
    /* Exec-failure marker: exit 127 with stderr exactly one NUL. */
    if (exit_code == 127 && elen == 1 && g_io_buf[olen] == '\0') {
      ev_fail(ev, line, col, "E5010", "file not found at runtime");
      return NULL;
    }
    ob = (unsigned char *)valloc(ev, olen > 0 ? olen : 1, line, col);
    eb = (unsigned char *)valloc(ev, elen > 0 ? elen : 1, line, col);
    if (ob == NULL || eb == NULL) {
      return NULL;
    }
    for (t = 0; t < olen; t++) {
      ob[t] = g_io_buf[t];
    }
    for (t = 0; t < elen; t++) {
      eb[t] = g_io_buf[olen + t];
    }
    if (!lex_valid_utf8(ob, olen) || !lex_valid_utf8(eb, elen)) {
      ev_fail(ev, line, col, "E5007", "non-UTF-8 process output");
      return NULL;
    }
    outv = vint(ev, (int64_t)exit_code, line, col);
    errv = vstr(ev, eb, elen, line, col);
    {
      Val *stdoutv = vstr(ev, ob, olen, line, col);
      if (outv == NULL || errv == NULL || stdoutv == NULL) {
        return NULL;
      }
      map = vnew(ev, V_MAP, line, col);
      if (map == NULL) {
        return NULL;
      }
      keys[0].p = (const unsigned char *)k_exit;
      keys[0].n = 4;
      keys[1].p = (const unsigned char *)k_out;
      keys[1].n = 6;
      keys[2].p = (const unsigned char *)k_err;
      keys[2].n = 6;
      vals[0] = outv;
      vals[1] = stdoutv;
      vals[2] = errv;
      {
        Slice *kcopy =
            (Slice *)valloc(ev, 3 * sizeof(Slice), line, col);
        Val **vcopy =
            (Val **)valloc(ev, 3 * sizeof(Val *), line, col);
        if (kcopy == NULL || vcopy == NULL) {
          return NULL;
        }
        for (t = 0; t < 3; t++) {
          kcopy[t] = keys[t];
          vcopy[t] = vals[t];
        }
      map->keys = kcopy;
      map->vals = vcopy;
      map->n = 3;
      }
      return map;
    }
  }
}

static Val *builtin_hash(Ev *ev, Val **args, size_t nargs, int line,
                         int col) {
  char hex[65];
  Val *list = need_list_arg(ev, args, nargs, line, col);
  if (list == NULL || !need_elems(ev, list, 1, line, col) ||
      !need_str(ev, list->items[0], line, col)) {
    return NULL;
  }
  sha256_hex(list->items[0]->s.p, list->items[0]->s.n, hex);
  {
    size_t t;
    unsigned char *copy;
    /* Hex output is pure ASCII by construction. */
    copy = (unsigned char *)valloc(ev, 64, line, col);
    if (copy == NULL) {
      return NULL;
    }
    for (t = 0; t < 64; t++) {
      copy[t] = (unsigned char)hex[t];
    }
    return vstr(ev, copy, 64, line, col);
  }
}

/* True when a resolved path stays inside the source root. Guards
   lockfile entries: pins pointing outside are never even read, so
   verification cannot become an oracle for outside content (M3). */
static int path_inside_root(const char *root, const char *resolved) {
  size_t i = 0;
  if (root[0] == '.' && root[1] == '\0') {
    if (resolved[0] == '/') {
      return 0;
    }
    if (resolved[0] == '.' && resolved[1] == '.' &&
        (resolved[2] == '/' || resolved[2] == '\0')) {
      return 0;
    }
    return 1;
  }
  while (root[i] != '\0') {
    if (resolved[i] != root[i]) {
      return 0;
    }
    i += 1;
  }
  return resolved[i] == '\0' || resolved[i] == '/';
}

/* lock.check([path]): verifies every pin of a lockfile against
   current content, anchored at the source root like fs paths.
   Returns true only when everything matches; missing, unreadable,
   or malformed input verifies false (never a failure). Recorded
   as R16, pending countersign. */
static Val *builtin_lock(Ev *ev, Val **args, size_t nargs, int line,
                         int col) {
  char actual[VOW_PATH_CAP];
  size_t len;
  size_t pos = 0;
  int ok = 1;
  char pin[64];
  Val *list = need_list_arg(ev, args, nargs, line, col);
  if (list == NULL || !need_elems(ev, list, 1, line, col) ||
      !need_str(ev, list->items[0], line, col)) {
    return NULL;
  }
  if (!need_no_nul(ev, list->items[0]->s, line, col)) {
    return NULL;
  }
  if (!resolve_fs(ev, list->items[0]->s, actual, sizeof(actual))) {
    return vbool(ev, 0, line, col);
  }
  if (!clause_covers_path(ev, "lock", "check", actual)) {
    ev_fail(ev, line, col, "E5004", "capability scope violation");
    return NULL;
  }
  if (io_read(actual, &len) != 1) {
    return vbool(ev, 0, line, col);
  }
  /* Walk "<64hex><spaces><path>" lines; "#" lines skipped. */
  while (pos < len) {
    size_t eol = pos;
    size_t t;
    size_t hlen;
    size_t pstart;
    char entry[VOW_PATH_CAP];
    size_t elen = 0;
    size_t flen = 0;
    unsigned char *content = NULL;
    char hex[65];
    while (eol < len && g_io_buf[eol] != '\n') {
      eol += 1;
    }
    if (eol > pos && g_io_buf[pos] != '#') {
      /* Split hash and path on whitespace runs. */
      t = pos;
      while (t < eol && g_io_buf[t] != ' ' && g_io_buf[t] != '\t') {
        t += 1;
      }
      hlen = t - pos;
      while (t < eol &&
             (g_io_buf[t] == ' ' || g_io_buf[t] == '\t')) {
        t += 1;
      }
      pstart = t;
      while (t < eol && g_io_buf[t] != '\r') {
        t += 1;
      }
      elen = t - pstart;
      if (hlen != 64 || elen == 0 || elen + 1 >= sizeof(entry)) {
        ok = 0;
        break;
      }
      /* Copy the pin out first: verifying the target overwrites
         the scratch buffer holding this lockfile. */
      for (t = 0; t < 64; t++) {
        pin[t] = (char)g_io_buf[pos + t];
      }
      for (t = 0; t < elen; t++) {
        entry[t] = (char)g_io_buf[pstart + t];
      }
      entry[elen] = '\0';
      /* Entry paths resolve against the lockfile's own directory
         when relative... at stage-0 they resolve against the
         source root, matching how the reference mints them. */
      {
        char full[VOW_PATH_CAP];
        size_t q = 0;
        size_t r = 0;
        /* Reuse resolve_fs with a slice over the entry text. */
        Slice es;
        es.p = (const unsigned char *)entry;
        es.n = elen;
        if (entry[0] == '/') {
          while (entry[q] != '\0' && q + 1 < sizeof(full)) {
            full[q] = entry[q];
            q += 1;
          }
          full[q] = '\0';
        } else {
          if (!resolve_fs(ev, es, full, sizeof(full))) {
            ok = 0;
            break;
          }
        }
        /* Root fence first: entries outside are never even read,
           so verification cannot become an oracle (M3). */
        if (!path_inside_root(ev->root, full)) {
          ok = 0;
          break;
        }
        if (io_read(full, &flen) != 1) {
          ok = 0;
          break;
        }
        content = g_io_buf;
        sha256_hex(content, flen, hex);
        for (r = 0; r < 64; r++) {
          if (hex[r] != pin[r]) {
            break;
          }
        }
        if (r != 64) {
          ok = 0;
          break;
        }
      }
    }
    pos = (eol < len) ? eol + 1 : eol;
    if (!tick(ev, line, col)) {
      return NULL;
    }
  }
  return vbool(ev, ok, line, col);
}

static Val *eval_expr(Ev *ev, Env *env, Expr *e);
static Val *eval_call_site(Ev *ev, Env *env, Expr *e);

/* Top-level constant memo table: evaluated lazily on first
   reference (never at import, per R9), then reused. Re-entrant
   evaluation is a dependency cycle, reported as E5006: evaluation
   cannot complete within any bound. Direct self-reference never
   reaches here; the checker excludes the name from its own
   initializer (E3009). */
#define EVAL_MAX_CONSTS 4096

typedef struct {
  Decl *decl;
  Module *home;
  Val *val;
  int state;
} ConstMemo;

static ConstMemo g_consts[EVAL_MAX_CONSTS];
static size_t g_nconsts = 0;

static ConstMemo *memo_find(Module *home, Decl *decl) {
  size_t i;
  for (i = 0; i < g_nconsts; i++) {
    if (g_consts[i].home == home && g_consts[i].decl == decl) {
      return &g_consts[i];
    }
  }
  return NULL;
}

void ev_memo_reset(void) {
  g_nconsts = 0;
}

static Val *eval_const(Ev *ev, Env *env, Module *home, Decl *decl,
                       int line, int col);

Val *ev_func_val(Ev *ev, Module *home, Decl *func, int line, int col) {
  Val *v = vnew(ev, V_FUNC, line, col);
  if (v == NULL) {
    return NULL;
  }
  v->func = func;
  v->home = home;
  return v;
}

/* Resolves one bare name: lexical chain first, then the home
   module's tops (functions become values, constants evaluate
   lazily). Returns null with E3009 set when unbound. */
static Val *resolve_name(Ev *ev, Env *env, Module *home, Slice name,
                         int line, int col) {
  Val *found;
  Decl *d;
  found = env_find(env, name);
  if (found != NULL) {
    return found;
  }
  for (d = home->decls; d != NULL; d = d->next) {
    if (d->kind != D_FUNC && d->kind != D_CONST) {
      continue;
    }
    {
      size_t i = 0;
      int same = 1;
      if (d->name.n != name.n) {
        continue;
      }
      for (i = 0; i < name.n; i++) {
        if (d->name.p[i] != name.p[i]) {
          same = 0;
          break;
        }
      }
      if (!same) {
        continue;
      }
    }
    if (d->kind == D_FUNC) {
      return ev_func_val(ev, home, d, line, col);
    }
    return eval_const(ev, env, home, d, line, col);
  }
  ev_fail(ev, line, col, "E3009", "unknown name");
  return NULL;
}

static Val *eval_const(Ev *ev, Env *env, Module *home, Decl *decl,
                       int line, int col) {
  ConstMemo *m = memo_find(home, decl);
  Val *v;
  (void)env;
  if (m != NULL) {
    if (m->state == 2) {
      return m->val;
    }
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (g_nconsts >= EVAL_MAX_CONSTS) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  m = &g_consts[g_nconsts];
  g_nconsts += 1;
  m->decl = decl;
  m->home = home;
  m->val = NULL;
  m->state = 1;
  /* Constants evaluate in the home top scope with no clause: their
     initializers are pure by construction (E3008 at check). */
  {
    EffectEntry *saved_clause = ev->clause;
    Module *saved_mod = ev->mod;
    int saved_test = ev->in_test;
    ev->mod = home;
    ev->clause = NULL;
    ev->in_test = 0;
    v = eval_expr(ev, NULL, decl->value);
    ev->mod = saved_mod;
    ev->clause = saved_clause;
    ev->in_test = saved_test;
  }
  if (v == NULL) {
    return NULL;
  }
  m->val = v;
  m->state = 2;
  return v;
}

Val *ev_call(Ev *ev, Val *fn, Val **args, size_t nargs, int line,
             int col) {
  Module *saved_mod;
  EffectEntry *saved_clause;
  int saved_test;
  Env *frame = NULL;
  Val *ret = NULL;
  int has_ret = 0;
  size_t i;
  if (fn->kind != V_FUNC) {
    ev_fail(ev, line, col, "E5008", "call of non-function value");
    return NULL;
  }
  if (nargs != fn->func->nparam) {
    ev_fail(ev, line, col, "E5012", "runtime type error");
    return NULL;
  }
  if (!ev->in_test && ev->depth > 0) {
    /* R10 against the executing clause. The entry call itself has
       no caller, so depth zero skips the check. */
    EffectEntry *w;
    for (w = fn->func->effects; w != NULL; w = w->next) {
      EffectEntry *v;
      int found = 0;
      for (v = ev->clause; v != NULL; v = v->next) {
        size_t t;
        if (v->nname != w->nname) {
          continue;
        }
        found = 1;
        for (t = 0; t < v->nname; t++) {
          if (v->names[t].n != w->names[t].n) {
            found = 0;
            break;
          }
          {
            size_t u;
            for (u = 0; u < v->names[t].n; u++) {
              if (v->names[t].p[u] != w->names[t].p[u]) {
                found = 0;
                break;
              }
            }
          }
          if (!found) {
            break;
          }
        }
        if (found) {
          break;
        }
      }
      if (!found) {
        ev_fail(ev, line, col, "E5009", "dynamic effect violation");
        return NULL;
      }
    }
  }
  if (ev->depth >= EV_MAX_DEPTH) {
    ev_fail(ev, line, col, "E5006", "resource limit breach");
    return NULL;
  }
  if (!tick(ev, line, col)) {
    return NULL;
  }
  for (i = 0; i < nargs; i++) {
    frame = env_put(ev, frame, fn->func->params[i], args[i], line,
                    col);
    if (frame == NULL) {
      return NULL;
    }
  }
  saved_mod = ev->mod;
  saved_clause = ev->clause;
  saved_test = ev->in_test;
  ev->mod = fn->home;
  ev->clause = fn->func->effects;
  ev->in_test = 0;
  ev->depth += 1;
  if (!ev_block(ev, frame, fn->func->body, &ret, &has_ret)) {
    ev->mod = saved_mod;
    ev->clause = saved_clause;
    ev->in_test = saved_test;
    ev->depth -= 1;
    return NULL;
  }
  ev->mod = saved_mod;
  ev->clause = saved_clause;
  ev->in_test = saved_test;
  ev->depth -= 1;
  if (!has_ret) {
    /* Falling off the end returns 0. The specification never pins
       this shape down; returning 0 is deterministic and simple.
       Recorded as R21, pending ratification. */
    return vint(ev, 0, line, col);
  }
  return ret;
}

/* Resolves and executes one call expression. Capability paths
   dispatch to builtins by shape; import members resolve through the
   check-recorded target; lexical names resolve to values. */
static Val *eval_call_site(Ev *ev, Env *env, Expr *e) {
  size_t i;
  if (cap_is_root(e->names, e->nname)) {
    Cap cap = cap_lookup(e->names, e->nname);
    Val **args = NULL;
    if (cap == CAP_NONE) {
      ev_fail(ev, e->line, e->col, "E3001", "unknown effect name");
      return NULL;
    }
    if (e->nargs > 0) {
      args = (Val **)valloc(ev, e->nargs * sizeof(Val *), e->line,
                            e->col);
      if (args == NULL) {
        return NULL;
      }
      for (i = 0; i < e->nargs; i++) {
        args[i] = eval_expr(ev, env, e->args[i]);
        if (args[i] == NULL) {
          return NULL;
        }
      }
    }
    switch (cap) {
    case CAP_FS_READ:
      return builtin_fs_read(ev, args, e->nargs, e->line, e->col);
    case CAP_FS_WRITE:
      return builtin_fs_write(ev, args, e->nargs, e->line, e->col);
    case CAP_FS_LIST:
      return builtin_fs_list(ev, args, e->nargs, e->line, e->col);
    case CAP_PROC_SPAWN:
      return builtin_spawn(ev, args, e->nargs, e->line, e->col);
    case CAP_HASH_SHA256:
      return builtin_hash(ev, args, e->nargs, e->line, e->col);
    case CAP_LOCK_CHECK:
      return builtin_lock(ev, args, e->nargs, e->line, e->col);
    default:
      ev_fail(ev, e->line, e->col, "E3001", "unknown effect name");
      return NULL;
    }
  }
  {
    Val *fn = NULL;
    Val **args = NULL;
    if (e->nname == 0) {
      Val *obj = eval_expr(ev, env, e->a);
      if (obj == NULL) {
        return NULL;
      }
      if (obj->kind != V_FUNC) {
        ev_fail(ev, e->line, e->col, "E5008",
                "call of non-function value");
        return NULL;
      }
      fn = obj;
    } else if (e->nname > 1) {
      /* Import member through the check-recorded target. */
      Decl *dd;
      Decl *member = NULL;
      Module *target = NULL;
      for (dd = ev->mod->decls; dd != NULL; dd = dd->next) {
        if (dd->kind == D_IMPORT && dd->target != NULL &&
            dd->name.n == e->names[0].n) {
          size_t t;
          int same = 1;
          for (t = 0; t < dd->name.n; t++) {
            if (dd->name.p[t] != e->names[0].p[t]) {
              same = 0;
              break;
            }
          }
          if (same) {
            target = dd->target;
            break;
          }
        }
      }
      if (target == NULL || e->nname != 2) {
        ev_fail(ev, e->line, e->col, "E3009", "unknown name");
        return NULL;
      }
      {
        Decl *q;
        for (q = target->decls; q != NULL; q = q->next) {
          if ((q->kind == D_FUNC || q->kind == D_CONST) &&
              q->name.n == e->names[1].n) {
            size_t t;
            int same = 1;
            for (t = 0; t < q->name.n; t++) {
              if (q->name.p[t] != e->names[1].p[t]) {
                same = 0;
                break;
              }
            }
            if (same) {
              member = q;
              break;
            }
          }
        }
      }
      if (member == NULL) {
        ev_fail(ev, e->line, e->col, "E3009", "unknown name");
        return NULL;
      }
      if (member->kind == D_FUNC) {
        fn = ev_func_val(ev, target, member, e->line, e->col);
      } else {
        fn = eval_const(ev, env, target, member, e->line, e->col);
      }
      if (fn == NULL) {
        return NULL;
      }
    } else {
      fn = resolve_name(ev, env, ev->mod, e->names[0], e->line,
                        e->col);
      if (fn == NULL) {
        return NULL;
      }
    }
    if (e->nargs > 0) {
      args = (Val **)valloc(ev, e->nargs * sizeof(Val *), e->line,
                            e->col);
      if (args == NULL) {
        return NULL;
      }
      for (i = 0; i < e->nargs; i++) {
        args[i] = eval_expr(ev, env, e->args[i]);
        if (args[i] == NULL) {
          return NULL;
        }
      }
    }
    return ev_call(ev, fn, args, e->nargs, e->line, e->col);
  }
}

static Val *eval_expr(Ev *ev, Env *env, Expr *e) {
  if (e == NULL || ev->failed || ev->assert_failed) {
    return NULL;
  }
  if (!tick(ev, e->line, e->col)) {
    return NULL;
  }
  switch (e->kind) {
  case E_CALL:
    return eval_call_site(ev, env, e);
  case E_BINARY: {
    Val *a = eval_expr(ev, env, e->a);
    Val *b;
    int64_t r;
    if (a == NULL) {
      return NULL;
    }
    b = eval_expr(ev, env, e->b);
    if (b == NULL) {
      return NULL;
    }
    switch (e->op) {
    case T_PLUS:
    case T_MINUS:
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
      if (a->kind != V_INT || b->kind != V_INT) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      if (e->op == T_PLUS) {
        if (!add_i64(a->ival, b->ival, &r)) {
          ev_fail(ev, e->line, e->col, "E5000",
                  "integer overflow");
          return NULL;
        }
        return vint(ev, r, e->line, e->col);
      }
      if (e->op == T_MINUS) {
        if (!sub_i64(a->ival, b->ival, &r)) {
          ev_fail(ev, e->line, e->col, "E5000",
                  "integer overflow");
          return NULL;
        }
        return vint(ev, r, e->line, e->col);
      }
      if (e->op == T_STAR) {
        if (!mul_i64(a->ival, b->ival, &r)) {
          ev_fail(ev, e->line, e->col, "E5000",
                  "integer overflow");
          return NULL;
        }
        return vint(ev, r, e->line, e->col);
      }
      if (b->ival == 0) {
        ev_fail(ev, e->line, e->col, "E5001",
                "division or modulo by zero");
        return NULL;
      }
      if (a->ival == (int64_t)(-9223372036854775807LL - 1) &&
          b->ival == -1) {
        ev_fail(ev, e->line, e->col, "E5000",
                "integer overflow");
        return NULL;
      }
      if (e->op == T_SLASH) {
        return vint(ev, a->ival / b->ival, e->line, e->col);
      }
      return vint(ev, a->ival % b->ival, e->line, e->col);
    case T_EQ:
      return vbool(ev, val_eq(a, b), e->line, e->col);
    case T_NEQ:
      return vbool(ev, !val_eq(a, b), e->line, e->col);
    case T_LT:
    case T_LTE:
    case T_GT:
    case T_GTE: {
      int c;
      if ((a->kind != V_INT && a->kind != V_STR) ||
          a->kind != b->kind) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      if (a->kind == V_INT) {
        c = (a->ival < b->ival) ? -1 : ((a->ival > b->ival) ? 1 : 0);
      } else {
        c = cmp_str(a->s, b->s);
      }
      switch (e->op) {
      case T_LT:
        return vbool(ev, c < 0, e->line, e->col);
      case T_LTE:
        return vbool(ev, c <= 0, e->line, e->col);
      case T_GT:
        return vbool(ev, c > 0, e->line, e->col);
      default:
        return vbool(ev, c >= 0, e->line, e->col);
      }
    }
    case T_AMPAMP:
    case T_PIPEPIPE: {
      int x;
      int y;
      Val *rb;
      if (a->kind != V_BOOL) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      x = (a->ival != 0);
      /* Short-circuit: the right side runs only when needed. */
      if (e->op == T_AMPAMP && !x) {
        return vbool(ev, 0, e->line, e->col);
      }
      if (e->op == T_PIPEPIPE && x) {
        return vbool(ev, 1, e->line, e->col);
      }
      rb = eval_expr(ev, env, e->b);
      if (rb == NULL) {
        return NULL;
      }
      if (rb->kind != V_BOOL) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      y = (rb->ival != 0);
      if (e->op == T_AMPAMP) {
        return vbool(ev, x && y, e->line, e->col);
      }
      return vbool(ev, x || y, e->line, e->col);
    }
    default:
      ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
      return NULL;
    }
  }
  case E_UNARY: {
    Val *a = eval_expr(ev, env, e->a);
    if (a == NULL) {
      return NULL;
    }
    if (e->op == T_BANG) {
      if (a->kind != V_BOOL) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      return vbool(ev, a->ival == 0, e->line, e->col);
    }
    if (a->kind != V_INT) {
      ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
      return NULL;
    }
    if (a->ival == (int64_t)(-9223372036854775807LL - 1)) {
      ev_fail(ev, e->line, e->col, "E5000", "integer overflow");
      return NULL;
    }
    return vint(ev, -a->ival, e->line, e->col);
  }
  case E_INDEX: {
    Val *a = eval_expr(ev, env, e->a);
    Val *b;
    if (a == NULL) {
      return NULL;
    }
    b = eval_expr(ev, env, e->b);
    if (b == NULL) {
      return NULL;
    }
    if (a->kind == V_LIST) {
      uint64_t idx;
      if (b->kind != V_INT || b->ival < 0) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      idx = (uint64_t)b->ival;
      if (idx >= (uint64_t)a->n) {
        ev_fail(ev, e->line, e->col, "E5002", "index out of range");
        return NULL;
      }
      return a->items[(size_t)idx];
    }
    if (a->kind == V_MAP) {
      size_t k;
      if (b->kind != V_STR) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      for (k = 0; k < a->n; k++) {
        size_t t = 0;
        int same = 1;
        if (a->keys[k].n != b->s.n) {
          continue;
        }
        for (t = 0; t < b->s.n; t++) {
          if (a->keys[k].p[t] != b->s.p[t]) {
            same = 0;
            break;
          }
        }
        if (same) {
          return a->vals[k];
        }
      }
      ev_fail(ev, e->line, e->col, "E5003", "missing map key");
      return NULL;
    }
    if (a->kind == V_STR) {
      uint64_t idx;
      if (b->kind != V_INT || b->ival < 0) {
        ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
        return NULL;
      }
      idx = (uint64_t)b->ival;
      if (idx >= (uint64_t)a->s.n) {
        ev_fail(ev, e->line, e->col, "E5002", "index out of range");
        return NULL;
      }
      return vstr(ev, a->s.p + (size_t)idx, 1, e->line, e->col);
    }
    ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
    return NULL;
  }
  case E_IDENT: {
    Val *v;
    if (e->nname > 1) {
      ev_fail(ev, e->line, e->col, "E3009", "unknown name");
      return NULL;
    }
    v = resolve_name(ev, env, ev->mod, e->names[0], e->line, e->col);
    return v;
  }
  case E_INT:
    return vint(ev, e->ival, e->line, e->col);
  case E_STR:
    return vstr(ev, e->text.p, e->text.n, e->line, e->col);
  case E_BOOL:
    return vbool(ev, e->ival != 0, e->line, e->col);
  case E_LIST: {
    Val *v;
    Expr *it;
    size_t k = 0;
    v = vnew(ev, V_LIST, e->line, e->col);
    if (v == NULL) {
      return NULL;
    }
    if (e->nitem > 0) {
      v->items =
          (Val **)valloc(ev, e->nitem * sizeof(Val *), e->line,
                         e->col);
      if (v->items == NULL) {
        return NULL;
      }
    } else {
      v->items = NULL;
    }
    for (it = e->items; it != NULL; it = it->next) {
      Val *one = eval_expr(ev, env, it);
      if (one == NULL) {
        return NULL;
      }
      v->items[k] = one;
      k += 1;
    }
    v->n = e->nitem;
    return v;
  }
  case E_MAP: {
    Val *v;
    Expr *it;
    size_t k = 0;
    v = vnew(ev, V_MAP, e->line, e->col);
    if (v == NULL) {
      return NULL;
    }
    if (e->nitem > 0) {
      v->keys =
          (Slice *)valloc(ev, e->nitem * sizeof(Slice), e->line,
                          e->col);
      v->vals =
          (Val **)valloc(ev, e->nitem * sizeof(Val *), e->line,
                         e->col);
      if (v->keys == NULL || v->vals == NULL) {
        return NULL;
      }
    } else {
      v->keys = NULL;
      v->vals = NULL;
    }
    for (it = e->items; it != NULL; it = it->next) {
      Val *one = eval_expr(ev, env, it);
      if (one == NULL) {
        return NULL;
      }
      v->keys[k] = e->keys[k];
      v->vals[k] = one;
      k += 1;
    }
    v->n = e->nitem;
    return v;
  }
  default:
    ev_fail(ev, e->line, e->col, "E5012", "runtime type error");
    return NULL;
  }
}

int ev_block(Ev *ev, Env *env, Stmt *body, Val **ret, int *has_ret) {
  Stmt *s;
  *ret = NULL;
  *has_ret = 0;
  for (s = body; s != NULL; s = s->next) {
    if (ev->failed || ev->assert_failed) {
      return 0;
    }
    if (!tick(ev, s->line, s->col)) {
      return 0;
    }
    switch (s->kind) {
    case S_LET: {
      Val *v = eval_expr(ev, env, s->e);
      if (v == NULL) {
        return 0;
      }
      env = env_put(ev, env, s->name, v, s->line, s->col);
      if (env == NULL) {
        return 0;
      }
      break;
    }
    case S_ASSERT: {
      Val *v = eval_expr(ev, env, s->e);
      if (v == NULL) {
        return 0;
      }
      if (v->kind != V_BOOL) {
        ev_fail(ev, s->line, s->col, "E5012", "runtime type error");
        return 0;
      }
      if (v->ival == 0) {
        ev->assert_line = s->line;
        ev->assert_col = s->col;
        ev->has_msg = (s->msg != NULL);
        ev->assert_msg.p = NULL;
        ev->assert_msg.n = 0;
        if (s->msg != NULL) {
          /* Evaluated before flagging: ev_expr stops once the
             assertion flag is set. Messages are string literals
             by grammar, so this cannot fail freshly. */
          Val *mv = eval_expr(ev, env, s->msg);
          if (mv != NULL && mv->kind == V_STR) {
            ev->assert_msg = mv->s;
          }
        }
        ev->assert_failed = 1;
        return 0;
      }
      break;
    }
    case S_EXPR: {
      Val *v = eval_expr(ev, env, s->e);
      if (v == NULL) {
        return 0;
      }
      break;
    }
    case S_IF: {
      Val *c = eval_expr(ev, env, s->e);
      if (c == NULL) {
        return 0;
      }
      if (c->kind != V_BOOL) {
        ev_fail(ev, s->line, s->col, "E5012", "runtime type error");
        return 0;
      }
      if (c->ival != 0) {
        if (!ev_block(ev, env, s->body, ret, has_ret)) {
          return 0;
        }
      } else if (s->els != NULL) {
        if (!ev_block(ev, env, s->els, ret, has_ret)) {
          return 0;
        }
      }
      if (*has_ret) {
        return 1;
      }
      break;
    }
    case S_FOR: {
      Val *seq = eval_expr(ev, env, s->e);
      size_t k;
      if (seq == NULL) {
        return 0;
      }
      if (seq->kind != V_LIST) {
        ev_fail(ev, s->line, s->col, "E5012", "runtime type error");
        return 0;
      }
      for (k = 0; k < seq->n; k++) {
        Env *loop;
        if (ev->failed || ev->assert_failed) {
          return 0;
        }
        if (!tick(ev, s->line, s->col)) {
          return 0;
        }
        loop = env_put(ev, env, s->name, seq->items[k], s->line,
                       s->col);
        if (loop == NULL) {
          return 0;
        }
        if (!ev_block(ev, loop, s->body, ret, has_ret)) {
          return 0;
        }
        if (*has_ret) {
          return 1;
        }
      }
      break;
    }
    case S_RETURN: {
      if (s->e == NULL) {
        *ret = vint(ev, 0, s->line, s->col);
      } else {
        *ret = eval_expr(ev, env, s->e);
      }
      if (*ret == NULL) {
        return 0;
      }
      *has_ret = 1;
      return 1;
    }
    }
  }
  return ev->failed == 0 && ev->assert_failed == 0;
}