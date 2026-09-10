#include "parse.h"

/* Error helpers: unexpected end of input is always E2005
   (unterminated construct); a wrong present token is E2004. */

static void parse_fail(Parser *p, Tok t, const char *code,
                       const char *msg) {
  if (!p->failed) {
    p->failed = 1;
    p->err_line = t.line;
    p->err_col = t.col;
    p->err_code = code;
    p->err_msg = msg;
  }
}

static void parse_fail_here(Parser *p, const char *code, const char *msg) {
  parse_fail(p, p->cur, code, msg);
}

static void advance(Parser *p) {
  if (p->failed) {
    return;
  }
  p->cur = lex_next(&p->lx);
  if (p->cur.kind == T_INVALID) {
    p->failed = 1;
    p->err_line = p->lx.err_line;
    p->err_col = p->lx.err_col;
    p->err_code = p->lx.err_code;
    p->err_msg = p->lx.err_msg;
  }
}

void parse_init(Parser *p, Arena *arena, const char *file,
                const unsigned char *src, size_t len) {
  p->arena = arena;
  p->file = file;
  p->depth = 0;
  p->failed = 0;
  p->err_line = 1;
  p->err_col = 1;
  p->err_code = "";
  p->err_msg = "";
  lex_init(&p->lx, src, len);
  advance(p);
}

/* Expects one token kind. Fails E2005 on end of input, E2004
   otherwise. Returns 1 on success. */
static int expect(Parser *p, TokKind kind) {
  if (p->failed) {
    return 0;
  }
  if (p->cur.kind == kind) {
    advance(p);
    return 1;
  }
  if (p->cur.kind == T_EOF) {
    parse_fail_here(p, "E2005", "unexpected end of input");
  } else {
    parse_fail_here(p, "E2004", "unexpected token");
  }
  return 0;
}

/* Expects a closing delimiter. Either end of input or a wrong token
   still inside the construct means it was left open: E2005 on EOF,
   E2004 otherwise. */
static int expect_close(Parser *p, TokKind kind) {
  if (p->failed) {
    return 0;
  }
  if (p->cur.kind == kind) {
    advance(p);
    return 1;
  }
  if (p->cur.kind == T_EOF) {
    parse_fail_here(p, "E2005", "unterminated construct");
  } else {
    parse_fail_here(p, "E2004", "unexpected token");
  }
  return 0;
}

static int at(Parser *p, TokKind kind) {
  return !p->failed && p->cur.kind == kind;
}

static Slice tok_text(Parser *p) {
  Slice s;
  s.p = p->cur.start;
  s.n = p->cur.len;
  return s;
}

static void *alloc(Parser *p, size_t n) {
  return arena_alloc(p->arena, n);
}

static Expr *new_expr(Parser *p, ExprKind kind) {
  Expr *e = (Expr *)alloc(p, sizeof(Expr));
  e->kind = kind;
  /* Default stamp: the current token heads the construct. Composite
     nodes overwrite this with the operand position below. */
  e->line = p->cur.line;
  e->col = p->cur.col;
  return e;
}

static Stmt *new_stmt(Parser *p, StmtKind kind) {
  Stmt *s = (Stmt *)alloc(p, sizeof(Stmt));
  s->kind = kind;
  s->line = p->cur.line;
  s->col = p->cur.col;
  return s;
}

static Decl *new_decl(Parser *p, DeclKind kind) {
  Decl *d = (Decl *)alloc(p, sizeof(Decl));
  d->kind = kind;
  d->line = p->cur.line;
  d->col = p->cur.col;
  return d;
}

static EffectEntry *new_entry(Parser *p) {
  EffectEntry *e = (EffectEntry *)alloc(p, sizeof(EffectEntry));
  e->line = p->cur.line;
  e->col = p->cur.col;
  return e;
}

/* Forward declarations for the expression hierarchy. */
static Expr *parse_expr(Parser *p);
static Stmt *parse_block(Parser *p);

/* Dotted path: ident ("." ident)*. The caller owns length checks. */
static Slice *parse_dotted(Parser *p, size_t *out_n) {
  Slice *names;
  size_t cap = 4;
  size_t n = 0;
  size_t i;
  names = (Slice *)alloc(p, cap * sizeof(Slice));
  if (!at(p, T_IDENT)) {
    parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                    p->cur.kind == T_EOF ? "unexpected end of input"
                                         : "unexpected token");
    return NULL;
  }
  for (;;) {
    if (n == cap) {
      Slice *grown;
      cap = cap * 2u;
      grown = (Slice *)alloc(p, cap * sizeof(Slice));
      for (i = 0; i < n; i++) {
        grown[i] = names[i];
      }
      names = grown;
    }
    names[n] = tok_text(p);
    n += 1;
    advance(p);
    if (!at(p, T_DOT)) {
      break;
    }
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
  }
  *out_n = n;
  return names;
}

/* effect_entry := dotted_ident ("[" string ("," string)* "]")?.
   Anything inside the brackets that is not a string literal is E2006
   (non-literal effect scope); end of input there is E2005. */
static EffectEntry *parse_effect_entry(Parser *p) {
  EffectEntry *e = new_entry(p);
  size_t cap = 4;
  size_t n = 0;
  size_t i;
  e->names = parse_dotted(p, &e->nname);
  if (p->failed) {
    return NULL;
  }
  e->args = (Slice *)alloc(p, cap * sizeof(Slice));
  if (!at(p, T_LBRACK)) {
    return e;
  }
  advance(p);
  if (at(p, T_RBRACK)) {
    advance(p);
    return e;
  }
  for (;;) {
    if (p->cur.kind == T_EOF) {
      parse_fail_here(p, "E2005", "unterminated construct");
      return NULL;
    }
    if (p->cur.kind != T_STR) {
      parse_fail_here(p, "E2006", "non-literal effect scope");
      return NULL;
    }
    if (n == cap) {
      Slice *grown;
      cap = cap * 2u;
      grown = (Slice *)alloc(p, cap * sizeof(Slice));
      for (i = 0; i < n; i++) {
        grown[i] = e->args[i];
      }
      e->args = grown;
    }
    e->args[n] = tok_text(p);
    n += 1;
    advance(p);
    if (at(p, T_COMMA)) {
      advance(p);
      continue;
    }
    if (at(p, T_RBRACK)) {
      advance(p);
      break;
    }
    if (p->cur.kind == T_EOF) {
      parse_fail_here(p, "E2005", "unterminated construct");
      return NULL;
    }
    parse_fail_here(p, "E2006", "non-literal effect scope");
    return NULL;
  }
  e->nargs = n;
  return e;
}

/* effect_clause := "effect" "[" effect_entry ("," effect_entry)* "]". */
static EffectEntry *parse_effect_clause(Parser *p) {
  EffectEntry *head = NULL;
  EffectEntry **tail = &head;
  if (!at(p, T_EFFECT)) {
    return NULL;
  }
  advance(p);
  if (!expect(p, T_LBRACK)) {
    return NULL;
  }
  for (;;) {
    EffectEntry *e = parse_effect_entry(p);
    if (p->failed) {
      return NULL;
    }
    *tail = e;
    tail = &e->next;
    if (at(p, T_COMMA)) {
      advance(p);
      continue;
    }
    break;
  }
  if (!expect_close(p, T_RBRACK)) {
    return NULL;
  }
  return head;
}

/* param_list := ident ("," ident)* (bare identifiers at stage-0;
   R11 pending ratification). */
static Slice *parse_params(Parser *p, size_t *out_n) {
  Slice *params;
  size_t cap = 4;
  size_t n = 0;
  size_t i;
  params = (Slice *)alloc(p, cap * sizeof(Slice));
  if (at(p, T_RPAREN)) {
    *out_n = 0;
    return params;
  }
  for (;;) {
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    if (n == cap) {
      Slice *grown;
      cap = cap * 2u;
      grown = (Slice *)alloc(p, cap * sizeof(Slice));
      for (i = 0; i < n; i++) {
        grown[i] = params[i];
      }
      params = grown;
    }
    params[n] = tok_text(p);
    n += 1;
    advance(p);
    if (at(p, T_COMMA)) {
      advance(p);
      continue;
    }
    break;
  }
  *out_n = n;
  return params;
}

static Expr *parse_primary(Parser *p) {
  Tok t;
  Expr *e;
  if (p->failed) {
    return NULL;
  }
  t = p->cur;
  switch (t.kind) {
  case T_IDENT: {
    /* A dotted callee path; a following "(" makes it a call in
       postfix handling. */
    size_t n = 0;
    e = new_expr(p, E_IDENT);
    e->names = parse_dotted(p, &n);
    if (p->failed) {
      return NULL;
    }
    e->nname = n;
    return e;
  }
  case T_TRUE:
  case T_FALSE:
    e = new_expr(p, E_BOOL);
    e->ival = (t.kind == T_TRUE) ? 1 : 0;
    advance(p);
    return e;
  case T_INT:
    e = new_expr(p, E_INT);
    e->ival = t.ival;
    advance(p);
    return e;
  case T_STR:
    e = new_expr(p, E_STR);
    e->text = tok_text(p);
    advance(p);
    return e;
  case T_LPAREN: {
    advance(p);
    e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    if (!expect_close(p, T_RPAREN)) {
      return NULL;
    }
    return e;
  }
  case T_LBRACK: {
    /* List literal: "[" (expr ("," expr)*)? "]". */
    Expr *head = NULL;
    Expr **tail = &head;
    size_t n = 0;
    int bl = p->cur.line;
    int bc = p->cur.col;
    advance(p);
    e = new_expr(p, E_LIST);
    e->line = bl;
    e->col = bc;
    if (at(p, T_RBRACK)) {
      advance(p);
      e->items = NULL;
      e->nitem = 0;
      return e;
    }
    for (;;) {
      Expr *item = parse_expr(p);
      if (p->failed) {
        return NULL;
      }
      *tail = item;
      tail = &item->next;
      n += 1;
      if (at(p, T_COMMA)) {
        advance(p);
        continue;
      }
      break;
    }
    if (!expect_close(p, T_RBRACK)) {
      return NULL;
    }
    e->items = head;
    e->nitem = n;
    return e;
  }
  case T_LBRACE: {
    /* Map literal: "{" (string ":" expr ("," string ":" expr)*)? "}".
       Duplicate keys are accepted here; the check slice rejects
       them (E3005). */
    Expr *vhead = NULL;
    Expr **vtail = &vhead;
    Slice *keys;
    int *klines;
    int *kcols;
    size_t kcap = 4;
    size_t n = 0;
    size_t i;
    int bl = p->cur.line;
    int bc = p->cur.col;
    advance(p);
    e = new_expr(p, E_MAP);
    e->line = bl;
    e->col = bc;
    keys = (Slice *)alloc(p, kcap * sizeof(Slice));
    klines = (int *)alloc(p, kcap * sizeof(int));
    kcols = (int *)alloc(p, kcap * sizeof(int));
    if (at(p, T_RBRACE)) {
      advance(p);
      e->items = NULL;
      e->keys = keys;
      e->key_lines = klines;
      e->key_cols = kcols;
      e->nitem = 0;
      return e;
    }
    for (;;) {
      Expr *val;
      if (!at(p, T_STR)) {
        parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                        p->cur.kind == T_EOF ? "unexpected end of input"
                                             : "unexpected token");
        return NULL;
      }
      if (n == kcap) {
        Slice *grown;
        int *grown_lines;
        int *grown_cols;
        kcap = kcap * 2u;
        grown = (Slice *)alloc(p, kcap * sizeof(Slice));
        grown_lines = (int *)alloc(p, kcap * sizeof(int));
        grown_cols = (int *)alloc(p, kcap * sizeof(int));
        for (i = 0; i < n; i++) {
          grown[i] = keys[i];
          grown_lines[i] = klines[i];
          grown_cols[i] = kcols[i];
        }
        keys = grown;
        klines = grown_lines;
        kcols = grown_cols;
      }
      keys[n] = tok_text(p);
      klines[n] = p->cur.line;
      kcols[n] = p->cur.col;
      advance(p);
      if (!expect(p, T_COLON)) {
        return NULL;
      }
      val = parse_expr(p);
      if (p->failed) {
        return NULL;
      }
      *vtail = val;
      vtail = &val->next;
      n += 1;
      if (at(p, T_COMMA)) {
        advance(p);
        continue;
      }
      break;
    }
    if (!expect_close(p, T_RBRACE)) {
      return NULL;
    }
    e->items = vhead;
    e->keys = keys;
    e->key_lines = klines;
    e->key_cols = kcols;
    e->nitem = n;
    return e;
  }
  default:
    parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                    p->cur.kind == T_EOF ? "unexpected end of input"
                                         : "unexpected token");
    return NULL;
  }
}

static Expr *parse_postfix(Parser *p) {
  Expr *e = parse_primary(p);
  if (p->failed) {
    return NULL;
  }
  for (;;) {
    if (at(p, T_LPAREN)) {
      Expr *call;
      Expr *head = NULL;
      Expr **tail = &head;
      size_t n = 0;
      advance(p);
      call = new_expr(p, E_CALL);
      call->line = e->line;
      call->col = e->col;
      call->a = e;
      /* A direct dotted callee folds its path into the call node.
         Calls on other primaries (parenthesized expressions, values)
         keep the object in place; rejecting them is check-slice
         business (R12 pending), not grammar. This runs before the
         empty-argument shortcut below so zero-argument calls fold
         too. */
      if (e->kind == E_IDENT) {
        call->names = e->names;
        call->nname = e->nname;
        call->a = NULL;
      }
      if (at(p, T_RPAREN)) {
        advance(p);
        call->args = NULL;
        call->nargs = 0;
        e = call;
        continue;
      }
      for (;;) {
        Expr *arg = parse_expr(p);
        if (p->failed) {
          return NULL;
        }
        *tail = arg;
        tail = &arg->next;
        n += 1;
        if (at(p, T_COMMA)) {
          advance(p);
          continue;
        }
        break;
      }
      if (!expect_close(p, T_RPAREN)) {
        return NULL;
      }
      {
        Expr **arr = NULL;
        size_t i = 0;
        Expr *it;
        if (n > 0) {
          arr = (Expr **)alloc(p, n * sizeof(Expr *));
          for (it = head; it != NULL; it = it->next) {
            arr[i] = it;
            i += 1;
          }
        }
        call->args = arr;
        call->nargs = n;
      }
      e = call;
      continue;
    }
    if (at(p, T_LBRACK)) {
      Expr *ix = new_expr(p, E_INDEX);
      ix->line = e->line;
      ix->col = e->col;
      advance(p);
      ix->a = e;
      ix->b = parse_expr(p);
      if (p->failed) {
        return NULL;
      }
      if (!expect_close(p, T_RBRACK)) {
        return NULL;
      }
      e = ix;
      continue;
    }
    return e;
  }
}

static Expr *parse_unary(Parser *p) {
  if (!p->failed && (at(p, T_BANG) || at(p, T_MINUS))) {
    int op = (p->cur.kind == T_BANG) ? T_BANG : T_MINUS;
    int line = p->cur.line;
    int col = p->cur.col;
    Expr *e;
    advance(p);
    e = new_expr(p, E_UNARY);
    e->line = line;
    e->col = col;
    e->op = op;
    e->a = parse_unary(p);
    if (p->failed) {
      return NULL;
    }
    return e;
  }
  return parse_postfix(p);
}

/* Left-associative binary levels without function pointers, which
   the subset bans. Level 6 binds loosest, level 1 tightest, level 0
   is unary. */
static int level_op(Parser *p, int level) {
  switch (level) {
  case 6:
    if (at(p, T_PIPEPIPE)) {
      return T_PIPEPIPE;
    }
    break;
  case 5:
    if (at(p, T_AMPAMP)) {
      return T_AMPAMP;
    }
    break;
  case 4:
    if (at(p, T_EQ)) {
      return T_EQ;
    }
    if (at(p, T_NEQ)) {
      return T_NEQ;
    }
    break;
  case 3:
    if (at(p, T_LT)) {
      return T_LT;
    }
    if (at(p, T_LTE)) {
      return T_LTE;
    }
    if (at(p, T_GT)) {
      return T_GT;
    }
    if (at(p, T_GTE)) {
      return T_GTE;
    }
    break;
  case 2:
    if (at(p, T_PLUS)) {
      return T_PLUS;
    }
    if (at(p, T_MINUS)) {
      return T_MINUS;
    }
    break;
  case 1:
    if (at(p, T_STAR)) {
      return T_STAR;
    }
    if (at(p, T_SLASH)) {
      return T_SLASH;
    }
    if (at(p, T_PERCENT)) {
      return T_PERCENT;
    }
    break;
  default:
    break;
  }
  return 0;
}

static Expr *parse_level(Parser *p, int level) {
  Expr *e;
  if (level == 0) {
    return parse_unary(p);
  }
  e = parse_level(p, level - 1);
  if (p->failed) {
    return NULL;
  }
  for (;;) {
    int op;
    Expr *rhs;
    Expr *node;
    if (p->failed) {
      return NULL;
    }
    op = level_op(p, level);
    if (op == 0) {
      return e;
    }
    advance(p);
    rhs = parse_level(p, level - 1);
    if (p->failed) {
      return NULL;
    }
    node = new_expr(p, E_BINARY);
    node->line = e->line;
    node->col = e->col;
    node->op = op;
    node->a = e;
    node->b = rhs;
    e = node;
  }
}

static Expr *parse_expr(Parser *p) {
  Expr *e;
  if (p->failed) {
    return NULL;
  }
  if (p->depth >= PARSE_MAX_DEPTH) {
    parse_fail_here(p, "E2004", "nesting too deep");
    return NULL;
  }
  p->depth += 1;
  e = parse_level(p, 6);
  p->depth -= 1;
  return e;
}

static Stmt *parse_stmt(Parser *p);

/* block := "{" stmt* "}". Depth-guarded like expressions. */
static Stmt *parse_block(Parser *p) {
  Stmt *head = NULL;
  Stmt **tail = &head;
  if (!expect(p, T_LBRACE)) {
    return NULL;
  }
  if (p->depth >= PARSE_MAX_DEPTH) {
    parse_fail_here(p, "E2004", "nesting too deep");
    return NULL;
  }
  p->depth += 1;
  while (!p->failed && !at(p, T_RBRACE) && !at(p, T_EOF)) {
    Stmt *s = parse_stmt(p);
    if (p->failed) {
      p->depth -= 1;
      return NULL;
    }
    *tail = s;
    tail = &s->next;
  }
  p->depth -= 1;
  if (!expect_close(p, T_RBRACE)) {
    return NULL;
  }
  return head;
}

static Stmt *parse_stmt(Parser *p) {
  if (p->failed) {
    return NULL;
  }
  if (at(p, T_LET)) {
    Stmt *s = new_stmt(p, S_LET);
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    s->name = tok_text(p);
    advance(p);
    if (!expect(p, T_ASSIGN)) {
      return NULL;
    }
    s->e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    return s;
  }
  if (at(p, T_ASSERT)) {
    Stmt *s = new_stmt(p, S_ASSERT);
    advance(p);
    s->e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    if (at(p, T_COMMA)) {
      advance(p);
      if (!at(p, T_STR)) {
        parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                        p->cur.kind == T_EOF ? "unexpected end of input"
                                             : "unexpected token");
        return NULL;
      }
      s->msg = parse_expr(p);
      if (p->failed) {
        return NULL;
      }
    }
    return s;
  }
  if (at(p, T_IF)) {
    Stmt *s = new_stmt(p, S_IF);
    advance(p);
    s->e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    s->body = parse_block(p);
    if (p->failed) {
      return NULL;
    }
    if (at(p, T_ELSE)) {
      advance(p);
      s->els = parse_block(p);
      if (p->failed) {
        return NULL;
      }
    }
    return s;
  }
  if (at(p, T_FOR)) {
    Stmt *s = new_stmt(p, S_FOR);
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    s->name = tok_text(p);
    advance(p);
    if (!at(p, T_IN)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    advance(p);
    s->e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    s->body = parse_block(p);
    if (p->failed) {
      return NULL;
    }
    return s;
  }
  if (at(p, T_RETURN)) {
    Stmt *s = new_stmt(p, S_RETURN);
    advance(p);
    /* Bare "return" ends the statement; anything else must be an
       expression on the same construct. A closing brace can never
       start an expression, so it safely terminates the statement. */
    if (!at(p, T_RBRACE) && p->cur.kind != T_EOF) {
      /* Disambiguate: "return }" and "return <eof>" mean bare return.
         Anything else parses as an expression. */
      s->e = parse_expr(p);
      if (p->failed) {
        return NULL;
      }
    }
    return s;
  }
  {
    Stmt *s = new_stmt(p, S_EXPR);
    s->e = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    return s;
  }
}

static Decl *parse_decl(Parser *p) {
  if (at(p, T_IMPORT)) {
    Decl *d = new_decl(p, D_IMPORT);
    advance(p);
    if (!at(p, T_STR)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->path = tok_text(p);
    advance(p);
    /* "as" is not a keyword: it arrives as a plain identifier and is
       matched by text. */
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    {
      Slice w = tok_text(p);
      if (w.n != 2 || w.p[0] != 'a' || w.p[1] != 's') {
        parse_fail_here(p, "E2004", "unexpected token");
        return NULL;
      }
    }
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->name = tok_text(p);
    advance(p);
    {
      Slice w;
      if (!at(p, T_IDENT)) {
        parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                        p->cur.kind == T_EOF ? "unexpected end of input"
                                             : "unexpected token");
        return NULL;
      }
      w = tok_text(p);
      if (w.n != 4 || w.p[0] != 'h' || w.p[1] != 'a' || w.p[2] != 's' ||
          w.p[3] != 'h') {
        parse_fail_here(p, "E2004", "unexpected token");
        return NULL;
      }
      advance(p);
    }
    if (!at(p, T_STR)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->hash = tok_text(p);
    advance(p);
    return d;
  }
  if (at(p, T_EFFECT)) {
    /* Ambiguity: "effect" starts an effect declaration only when
       followed by ident "(". An effect clause appears only after a
       parameter list, never at declaration head... except "effect"
       alone is never a declaration. Peek: ident then lparen. */
    Decl *d;
    Lexer saved;
    Tok t1;
    Tok t2;
    saved = p->lx;
    /* Lookahead of two raw tokens without disturbing state. A lex
       error inside the lookahead is reported as-is once the tokens
       are really consumed, so propagate it instead of masking it. */
    t1 = lex_next(&p->lx);
    if (p->lx.failed) {
      p->failed = 1;
      p->err_line = p->lx.err_line;
      p->err_col = p->lx.err_col;
      p->err_code = p->lx.err_code;
      p->err_msg = p->lx.err_msg;
      return NULL;
    }
    t2 = lex_next(&p->lx);
    if (p->lx.failed) {
      p->failed = 1;
      p->err_line = p->lx.err_line;
      p->err_col = p->lx.err_col;
      p->err_code = p->lx.err_code;
      p->err_msg = p->lx.err_msg;
      return NULL;
    }
    p->lx = saved;
    if (t1.kind == T_IDENT && t2.kind == T_LPAREN) {
      d = new_decl(p, D_EFFECT);
      advance(p);
      d->name = tok_text(p);
      advance(p);
      if (!expect(p, T_LPAREN)) {
        return NULL;
      }
      d->params = parse_params(p, &d->nparam);
      if (p->failed) {
        return NULL;
      }
      if (!expect_close(p, T_RPAREN)) {
        return NULL;
      }
      return d;
    }
    parse_fail_here(p, "E2004", "unexpected token");
    return NULL;
  }
  if (at(p, T_LET)) {
    Decl *d = new_decl(p, D_CONST);
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->name = tok_text(p);
    advance(p);
    if (!expect(p, T_ASSIGN)) {
      return NULL;
    }
    /* Top-level initializers parse as full expressions; purity is a
       check-slice rule (E3008), not a grammar rule. */
    d->value = parse_expr(p);
    if (p->failed) {
      return NULL;
    }
    return d;
  }
  if (at(p, T_FUNC)) {
    Decl *d = new_decl(p, D_FUNC);
    advance(p);
    if (!at(p, T_IDENT)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->name = tok_text(p);
    advance(p);
    if (!expect(p, T_LPAREN)) {
      return NULL;
    }
    d->params = parse_params(p, &d->nparam);
    if (p->failed) {
      return NULL;
    }
    if (!expect_close(p, T_RPAREN)) {
      return NULL;
    }
    d->effects = parse_effect_clause(p);
    if (p->failed) {
      return NULL;
    }
    d->body = parse_block(p);
    if (p->failed) {
      return NULL;
    }
    return d;
  }
  if (at(p, T_TEST)) {
    Decl *d = new_decl(p, D_TEST);
    advance(p);
    if (!at(p, T_STR)) {
      parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                      p->cur.kind == T_EOF ? "unexpected end of input"
                                           : "unexpected token");
      return NULL;
    }
    d->name = tok_text(p);
    advance(p);
    d->body = parse_block(p);
    if (p->failed) {
      return NULL;
    }
    return d;
  }
  parse_fail_here(p, p->cur.kind == T_EOF ? "E2005" : "E2004",
                  p->cur.kind == T_EOF ? "unexpected end of input"
                                        : "unexpected token");
  return NULL;
}

Decl *parse_program(Parser *p) {
  Decl *head = NULL;
  Decl **tail = &head;
  while (!p->failed && !at(p, T_EOF)) {
    Decl *d = parse_decl(p);
    if (p->failed) {
      return NULL;
    }
    *tail = d;
    tail = &d->next;
  }
  if (p->failed) {
    return NULL;
  }
  return head;
}
