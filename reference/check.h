#ifndef VOW_CHECK_H
#define VOW_CHECK_H

#include "ast.h"
#include "lex.h"
#include "parse.h"

/* Slice-2 checker: purity, effect resolution with transitivity (R10),
   import verification (existence, pin, cycle, root escape), duplicate
   map keys, duplicate bindings, unknown names, and the reserved
   capability roots. Evaluation belongs to a later slice. */

/* Closed capability allowlist (stage-0 specification, Section 7). */
typedef enum {
  CAP_NONE,
  CAP_FS_READ,
  CAP_FS_WRITE,
  CAP_FS_LIST,
  CAP_PROC_SPAWN,
  CAP_HASH_SHA256,
  CAP_LOCK_CHECK
} Cap;

/* Contract: maps a dotted path to its capability, or CAP_NONE. */
Cap cap_lookup(const Slice *names, size_t n);

/* Contract: true when the head segment reserves a capability root
   (fs, proc, hash, lock), whether or not the full path is known. */
int cap_is_root(const Slice *names, size_t n);

#define CHECK_MAX_MODULES 256
#define CHECK_LOAD_DEPTH 64
#define VOW_PATH_CAP 4096

typedef struct Module Module;

/* One loaded module: its display path, declarations, and top-level
   symbol index state. */
struct Module {
  char path[VOW_PATH_CAP];
  Decl *decls;
  int done;
};

/* The checker owns one arena (shared with lex/parse), the module
   cache, the loading stack for cycle detection, and the first
   error. */
typedef struct {
  Arena *arena;
  char root[VOW_PATH_CAP];
  Module *modules[CHECK_MAX_MODULES];
  size_t nmod;
  const char *loading[CHECK_LOAD_DEPTH];
  size_t nloading;
  int failed;
  char err_file[VOW_PATH_CAP];
  int err_line;
  int err_col;
  const char *err_code;
  const char *err_msg;
} Checker;

/* Contract: prepares a checker. The source root is derived from
   each entry path by check_file. No failure modes. */
void check_init(Checker *c, Arena *arena);

/* Contract: lexes, parses, and checks one entry file plus everything
   it imports. Returns 1 on success. On failure returns 0 with
   err_file/err_line/err_col/err_code/err_msg set (E2xxx from lexing
   or parsing, E3xxx from checking). When require_main is true, a
   missing zero-parameter main is E3007. */
int check_file(Checker *c, const char *path, int require_main);

/* Contract: same as check_file, but the entry bytes come from memory
   (the ABI stub embeds them) instead of disk. display names the
   entry for diagnostics; root is the absolute source root that
   imports and fs paths resolve against. Returns 1 on success, 0
   with the checker error fields set on failure. */
int check_memory(Checker *c, const char *display, const char *root,
                 const unsigned char *src, size_t len,
                 int require_main);

#endif
