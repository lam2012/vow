#ifndef VOW_AST_H
#define VOW_AST_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration: the checker fills this for D_IMPORT. */
typedef struct Module Module;

/* A byte slice borrowing from the source buffer. */
typedef struct {
  const unsigned char *p;
  size_t n;
} Slice;

/* Bump arena with fixed capacity. Exhaustion ends the process with a
   diagnostic (never silent truncation); arenas are released as a
   whole, so there is no ownership tracking. */
typedef struct {
  unsigned char *base;
  size_t cap;
  size_t used;
} Arena;

/* Contract: binds an arena to a caller-owned buffer. No failure modes. */
void arena_init(Arena *a, unsigned char *base, size_t cap);

/* Contract: rounds n up to the strictest fundamental alignment. */
size_t arena_align_up(size_t n);

/* Contract: returns n zeroed bytes or ends the process with E5006
   when the arena is exhausted. n must be greater than zero. */
void *arena_alloc(Arena *a, size_t n);

typedef enum {
  E_BINARY, E_UNARY, E_CALL, E_INDEX,
  E_IDENT, E_INT, E_STR, E_BOOL, E_LIST, E_MAP
} ExprKind;

typedef struct Expr Expr;
struct Expr {
  ExprKind kind;
  int line;                  /* 1-based position of the construct head */
  int col;                   /* byte column, 1-based */
  int op;                    /* token kind for operators, else 0 */
  Expr *a;                   /* left operand / callee object / indexed */
  Expr *b;                   /* right operand / index */
  Slice *names;              /* dotted callee path segments */
  size_t nname;
  Expr **args;               /* call arguments */
  size_t nargs;
  Expr *items;               /* list elements or map values, linked */
  Slice *keys;               /* map keys, parallel to values */
  int *key_lines;            /* map key positions, parallel to keys */
  int *key_cols;
  size_t nitem;
  Slice text;                /* identifier or string literal */
  int64_t ival;              /* integer literal or bool value */
  Expr *next;                /* list linkage */
};

typedef enum {
  S_LET, S_ASSERT, S_EXPR, S_IF, S_FOR, S_RETURN
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
  StmtKind kind;
  int line;
  int col;
  Slice name;                /* let/for binding */
  Expr *e;                   /* value, condition, iterated, returned */
  Expr *msg;                 /* assert message or null */
  Stmt *body;                /* then/for body */
  Stmt *els;                 /* else body or null */
  Stmt *next;                /* list linkage */
};

typedef struct EffectEntry EffectEntry;
struct EffectEntry {
  int line;
  int col;
  Slice *names;              /* dotted capability path segments */
  size_t nname;
  Slice *args;               /* declared string scopes */
  size_t nargs;
  EffectEntry *next;
};

typedef enum {
  D_IMPORT, D_EFFECT, D_CONST, D_FUNC, D_TEST
} DeclKind;

typedef struct Decl Decl;
struct Decl {
  DeclKind kind;
  int line;
  int col;
  Slice name;                /* alias, effect/capability, const, func */
  Slice path;                /* import target */
  Slice hash;                /* import pin */
  struct Module *target;     /* filled by the checker for D_IMPORT */
  Slice *params;             /* parameter names */
  size_t nparam;
  EffectEntry *effects;      /* declared clause or null */
  Expr *value;               /* const initializer */
  Stmt *body;                /* func/test body */
  Decl *next;                /* list linkage */
};

#endif
