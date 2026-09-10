#ifndef VOW_EVAL_H
#define VOW_EVAL_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "check.h"

/* Slice-3 evaluator: executes checked programs. Values are
   dynamically typed; every type mismatch is E5012, every resource
   breach E5006. */

/* Default limits from the stage-0 specification. */
#define EV_MAX_STEPS ((uint64_t)100000000u)
#define EV_MAX_DEPTH 1024

typedef enum {
  V_INT,
  V_STR,
  V_BOOL,
  V_LIST,
  V_MAP,
  V_FUNC
} VKind;

typedef struct Val Val;
struct Val {
  VKind kind;
  int64_t ival;
  Slice s;        /* string bytes */
  Val **items;    /* list elements */
  Slice *keys;    /* map keys */
  Val **vals;     /* map values, parallel to keys */
  size_t n;
  Decl *func;     /* function declaration for V_FUNC */
  Module *home;   /* defining module for V_FUNC */
};

/* Lexical environment: a chain of single bindings. Nodes live in
   the value pool and are never freed (short-lived processes;
   exhaustion is E5006, documented in the README). */
typedef struct Env Env;
struct Env {
  Slice name;
  Val *val;
  Env *parent;
};

typedef struct {
  Checker *chk;
  Module *mod;        /* module of the code under execution */
  const char *root;   /* source root for path anchoring */
  EffectEntry *clause; /* executing function's clause, null in tests */
  int in_test;
  uint64_t steps;
  int depth;
  int failed;
  char err_file[VOW_PATH_CAP];
  int err_line;
  int err_col;
  const char *err_code;
  const char *err_msg;
  /* Assertion failure is distinct from diagnostics: the test is
     false, nothing is broken. Run.c reads these. */
  int assert_failed;
  int assert_line;
  int assert_col;
  Slice assert_msg;
  int has_msg;
} Ev;

/* Contract: prepares an evaluator over already-checked modules.
   root anchors relative fs paths. No failure modes. */
void ev_init(Ev *ev, Checker *chk, Module *entry, const char *root);

/* Contract: calls a function value with argument values. Returns the
   return value, or null with an error set (E3000/E5008/E5009/E5006
   and callee-raised failures). Enforces R10 dynamically for every
   user-function call against the caller's clause. */
Val *ev_call(Ev *ev, Val *fn, Val **args, size_t nargs, int line,
             int col);

/* Contract: wraps a declaration as a first-class function value.
   Returns null with E5006 set when the pool is exhausted. */
Val *ev_func_val(Ev *ev, Module *home, Decl *func, int line, int col);

/* Contract: clears the top-level constant memo table. Callers reset
   once per checked file: module and declaration pointers are only
   unique within one arena lifetime, so memo entries must never cross
   files (M1). No failure modes. */
void ev_memo_reset(void);

/* Contract: executes a statement list. Return statements set
   *ret to the value and *has_ret to 1. Returns 1 unless a failure
   is set. Expression evaluation stays internal to eval.c. */
int ev_block(Ev *ev, Env *env, Stmt *body, Val **ret, int *has_ret);

#endif
