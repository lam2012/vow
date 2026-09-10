#include "run.h"

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "eval.h"
#include "lex.h"
#include "parse.h"
#include "platform.h"

/* Bounded string buffer helpers (no snprintf dependency games, no
   truncation surprises: overlong paths fail loudly). */

static int path_join(char *out, size_t cap, const char *a,
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
  /* Exactly one separator: a trailing slash on the parent does not
     duplicate. Display paths stay byte-stable for hash comparison. */
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

static int str_eq(const char *a, const char *b) {
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

static int ends_vow(const char *name) {
  size_t n = 0;
  while (name[n] != '\0') {
    n += 1;
  }
  return n > 4 && name[n - 4] == '.' && name[n - 3] == 'v' &&
         name[n - 2] == 'o' && name[n - 1] == 'w';
}

#define RUN_MAX_FILES 4096
#define RUN_PATH_CAP 4096

typedef struct {
  char paths[RUN_MAX_FILES][RUN_PATH_CAP];
  size_t n;
} FileList;

static int collect_one(const char *dir, FileList *out) {
  PlatDir d;
  struct dirent *ent;
  if (plat_opendir(dir, &d) != 0) {
    /* Missing or unreadable directories contribute no files;
       only resource exhaustion propagates (M3 nit: RUN_MAX_FILES
       should be E5006, not E1003; fixed here). */
    return (plat_err() == PLAT_RESOURCE) ? -1 : 0;
  }
  for (;;) {
    ent = plat_readdir(&d);
    if (ent == NULL) {
      if (plat_err() != PLAT_OK) {
        plat_closedir(&d);
        return -1;
      }
      break;
    }
    if (ent->d_name[0] == '.') {
      /* Skips "." and ".." along with every other dotfile. */
      continue;
    }
    if (ent->d_name[0] == '_') {
      continue;
    }
    if (str_eq(ent->d_name, "fail")) {
      /* Negative cases run through driver assertions only (R17). */
      continue;
    }
    {
      char child[RUN_PATH_CAP];
      if (!path_join(child, sizeof(child), dir, ent->d_name)) {
        plat_closedir(&d);
        return -1;
      }
      if (ends_vow(ent->d_name)) {
        size_t k = 0;
        if (out->n >= RUN_MAX_FILES) {
          plat_closedir(&d);
          return -1;
        }
        while (child[k] != '\0') {
          out->paths[out->n][k] = child[k];
          k += 1;
        }
        out->paths[out->n][k] = '\0';
        out->n += 1;
      } else {
        /* Descend into anything else that is a directory; regular
           non-vow files fail the recursion below harmlessly... except
           opendir on a regular file fails, which collect_one
           reports as 0 (skip). Distinguish: try open, skip on
           failure only when errno says so? Simpler: attempt
           recursion and ignore its 0, but propagate -1. */
        int rc = collect_one(child, out);
        if (rc < 0) {
          plat_closedir(&d);
          return -1;
        }
      }
    }
  }
  plat_closedir(&d);
  return 1;
}

/* Byte-wise path order for deterministic collection. */
static void sort_files(FileList *out) {
  size_t i;
  size_t j;
  for (i = 0; i < out->n; i++) {
    for (j = i + 1; j < out->n; j++) {
      size_t k = 0;
      int swap = 0;
      while (out->paths[i][k] != '\0' && out->paths[j][k] != '\0') {
        if (out->paths[i][k] != out->paths[j][k]) {
          swap = (out->paths[i][k] > out->paths[j][k]) ? 1 : 0;
          break;
        }
        k += 1;
      }
      if (!swap && out->paths[i][k] != '\0' &&
          out->paths[j][k] == '\0') {
        swap = 1;
      }
      if (swap) {
        char tmp[RUN_PATH_CAP];
        size_t t = 0;
        while (out->paths[i][t] != '\0') {
          tmp[t] = out->paths[i][t];
          t += 1;
        }
        tmp[t] = '\0';
        t = 0;
        while (out->paths[j][t] != '\0') {
          out->paths[i][t] = out->paths[j][t];
          t += 1;
        }
        out->paths[i][t] = '\0';
        t = 0;
        while (tmp[t] != '\0') {
          out->paths[j][t] = tmp[t];
          t += 1;
        }
        out->paths[j][t] = '\0';
      }
    }
  }
}

static unsigned char g_run_arena[8 * 1024 * 1024];

typedef struct {
  int pass;
  int fail;
} Counts;

static void report_ok(const char *file, int line, Slice ns) {
  size_t i;
  printf("ok ");
  for (i = 0; i < ns.n; i++) {
    putchar((int)ns.p[i]);
  }
  printf(" %s:%d\n", file, line);
}

static void report_not_ok(const char *file, int line, Slice ns,
                          const char *detail) {
  size_t i;
  printf("not ok ");
  for (i = 0; i < ns.n; i++) {
    putchar((int)ns.p[i]);
  }
  printf(" %s:%d", file, line);
  if (detail != NULL) {
    printf(" %s", detail);
  }
  putchar('\n');
}

/* Executes one file's test blocks under the suite root: checking
   stays per-file (imports resolve against the file's own dir), but
   evaluation anchors fs paths at the suite root so declared scopes
   mean the same tree for every file. Broken files report file-level
   not-ok entries instead of aborting the run. */
static void run_file_tests(const char *root, const char *path,
                           Counts *counts) {
  Arena arena;
  Checker file_chk;
  Module *entry = NULL;
  size_t i;
  Slice noname;
  noname.p = (const unsigned char *)"file";
  noname.n = 4;
  arena_init(&arena, g_run_arena, sizeof(g_run_arena));
  check_init(&file_chk, &arena);
  /* Fresh memo per file: module and declaration pointers repeat
     across arena lifetimes (M1). */
  ev_memo_reset();
  if (!check_file(&file_chk, path, 0)) {
    char detail[256];
    size_t t = 0;
    size_t u = 0;
    while (file_chk.err_code[t] != '\0' && t + 1 < sizeof(detail)) {
      detail[t] = file_chk.err_code[t];
      t += 1;
    }
    if (t + 1 < sizeof(detail)) {
      detail[t] = ' ';
      t += 1;
    }
    while (file_chk.err_msg[u] != '\0' && t + 1 < sizeof(detail)) {
      detail[t] = file_chk.err_msg[u];
      t += 1;
      u += 1;
    }
    detail[t] = '\0';
    report_not_ok(path, file_chk.err_line, noname, detail);
    counts->fail += 1;
    return;
  }
  /* Find the entry module (last loaded with this exact path). */
  for (i = 0; i < file_chk.nmod; i++) {
    const char *p = file_chk.modules[i]->path;
    size_t j = 0;
    int same = 1;
    for (;;) {
      if (p[j] != path[j]) {
        same = 0;
        break;
      }
      if (p[j] == '\0') {
        break;
      }
      j += 1;
    }
    if (same) {
      entry = file_chk.modules[i];
      break;
    }
  }
  if (entry == NULL) {
    report_not_ok(path, 1, noname, "E5006 resource limit breach");
    counts->fail += 1;
    return;
  }
  {
    Ev run;
    Decl *d;
    ev_init(&run, &file_chk, entry, root);
    for (d = entry->decls; d != NULL; d = d->next) {
      if (d->kind != D_TEST) {
        continue;
      }
      {
        Val *ret = NULL;
        int has_ret = 0;
        run.assert_failed = 0;
        run.failed = 0;
        run.in_test = 1;
        run.mod = entry;
        run.clause = NULL;
        if (!ev_block(&run, NULL, d->body, &ret, &has_ret)) {
          if (run.assert_failed) {
            if (run.has_msg && run.assert_msg.p != NULL) {
              size_t t;
              char msg[256];
              size_t m = 0;
              for (t = 0; t < run.assert_msg.n && m + 1 < sizeof(msg);
                   t++) {
                msg[m] = (char)run.assert_msg.p[t];
                m += 1;
              }
              msg[m] = '\0';
              report_not_ok(path, run.assert_line, d->name, msg);
            } else {
              report_not_ok(path, run.assert_line, d->name, NULL);
            }
          } else {
            char detail[256];
            size_t t = 0;
            size_t u = 0;
            while (run.err_code[t] != '\0' && t + 1 < sizeof(detail)) {
              detail[t] = run.err_code[t];
              t += 1;
            }
            if (t + 1 < sizeof(detail)) {
              detail[t] = ' ';
              t += 1;
            }
            while (run.err_msg[u] != '\0' && t + 1 < sizeof(detail)) {
              detail[t] = run.err_msg[u];
              t += 1;
              u += 1;
            }
            detail[t] = '\0';
            report_not_ok(path, run.err_line, d->name, detail);
          }
          counts->fail += 1;
        } else {
          report_ok(path, d->line, d->name);
          counts->pass += 1;
        }
      }
    }
  }
}

int run_tests(const char *vowpath, const char *dir) {
  static FileList files;
  char root[RUN_PATH_CAP];
  size_t i;
  size_t n = 0;
  Counts counts;
  (void)vowpath;
  counts.pass = 0;
  counts.fail = 0;
  files.n = 0;
  /* Normalize the suite root once (no trailing slash): every
     collected display path and every fs anchor derives from it. */
  while (dir[n] != '\0' && n + 1 < sizeof(root)) {
    root[n] = dir[n];
    n += 1;
  }
  if (dir[n] != '\0') {
    printf("not ok %s:1 E5006 resource limit breach\n", dir);
    return 4;
  }
  root[n] = '\0';
  while (n > 1 && root[n - 1] == '/') {
    n -= 1;
    root[n] = '\0';
  }
  {
    /* Missing target is a usage error (E1003); an unreadable one
       is a resource failure (E5006). Overflow during collection
       is E5006 as well. Sort steps are bounded by RUN_MAX_FILES,
       so no tick is needed here: the bound is the gate. */
    const char *target = (root[0] == '\0') ? "." : root;
    int probe = plat_probe_dir(target);
    int rc;
    if (!probe) {
      if (plat_err() == PLAT_MISSING) {
        fprintf(stderr, "%s:1:1: E1003 target directory missing\n",
                target);
        return 1;
      }
      fprintf(stderr, "%s:1:1: E5006 resource limit breach\n",
              target);
      return 5;
    }
    rc = collect_one(target, &files);
    if (rc < 0) {
      fprintf(stderr, "%s:1:1: E5006 resource limit breach\n",
              target);
      return 5;
    }
    if (rc == 0) {
      fprintf(stderr, "%s:1:1: E1003 target directory missing\n",
              target);
      return 1;
    }
  }
  sort_files(&files);
  for (i = 0; i < files.n; i++) {
    run_file_tests(root[0] == '\0' ? "." : root, files.paths[i],
                   &counts);
  }
  printf("pass: %d fail: %d\n", counts.pass, counts.fail);
  return (counts.fail == 0) ? 0 : 4;
}

int run_main(const char *vowpath, const char *file) {
  static unsigned char arena_buf[8 * 1024 * 1024];
  Arena arena;
  Checker chk;
  Module *entry = NULL;
  size_t i;
  (void)vowpath;
  arena_init(&arena, arena_buf, sizeof(arena_buf));
  check_init(&chk, &arena);
  ev_memo_reset();
  if (!check_file(&chk, file, 1)) {
    fprintf(stderr, "%s:%d:%d: %s %s\n", chk.err_file, chk.err_line,
            chk.err_col, chk.err_code, chk.err_msg);
    {
      char c = chk.err_code[1];
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
  }
  for (i = 0; i < chk.nmod; i++) {
    const char *p = chk.modules[i]->path;
    size_t j = 0;
    int same = 1;
    for (;;) {
      if (p[j] != file[j]) {
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
    fprintf(stderr, "%s:1:1: E5006 resource limit breach\n", file);
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
              file);
      return 3;
    }
    {
      Val *r = ev_call(&ev, mainv, NULL, 0, 1, 1);
      if (r == NULL) {
        fprintf(stderr, "%s:%d:%d: %s %s\n", ev.err_file,
                ev.err_line, ev.err_col, ev.err_code, ev.err_msg);
        {
          char c = ev.err_code[1];
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
      }
      if (r->kind != V_INT) {
        fprintf(stderr, "%s:1:1: E5011 main must return an integer\n",
                file);
        return 5;
      }
      return (int)(r->ival & 0xFF);
    }
  }
}
