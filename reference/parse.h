#ifndef VOW_PARSE_H
#define VOW_PARSE_H

#include "ast.h"
#include "lex.h"

/* Single-token-lookahead recursive descent parser. It enforces grammar
   only: purity, effect resolution, import verification, and main
   presence belong to later check slices. */

/* Maximum combined nesting of blocks and expressions. Exceeding it is
   a parse error (E2004), never a stack overflow. */
#define PARSE_MAX_DEPTH 1024

typedef struct {
  Lexer lx;
  Tok cur;
  int depth;
  int failed;
  int err_line;
  int err_col;
  const char *err_code;
  const char *err_msg;
  Arena *arena;
  const char *file;
} Parser;

/* Contract: prepares a parser over src[0..len) from the named file.
   No failure modes. */
void parse_init(Parser *p, Arena *arena, const char *file,
                const unsigned char *src, size_t len);

/* Contract: parses a whole program. Returns the declaration list, or
   null when parsing fails; then err_code/err_msg/err_line/err_col
   describe the first error (E2004 unexpected token, E2005
   unterminated construct, E2006 non-literal effect scope; lex errors
   propagate with their own codes). */
Decl *parse_program(Parser *p);

#endif
