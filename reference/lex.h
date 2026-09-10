#ifndef VOW_LEX_H
#define VOW_LEX_H

#include <stddef.h>
#include <stdint.h>

/* Token kinds: keywords, symbols, literals, end of input, and one
   invalid kind that carries a lex error (E2000–E2005). */
typedef enum {
  T_EOF,
  T_IDENT,
  T_INT,
  T_STR,
  T_FUNC, T_LET, T_IF, T_ELSE, T_RETURN,
  T_TRUE, T_FALSE, T_IMPORT, T_EFFECT, T_TEST,
  T_ASSERT, T_FOR, T_IN,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
  T_LBRACK, T_RBRACK, T_COMMA, T_COLON,
  T_ASSIGN, T_EQ, T_NEQ, T_LT, T_LTE, T_GT, T_GTE,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
  T_BANG, T_AMPAMP, T_PIPEPIPE, T_DOT,
  T_INVALID
} TokKind;

/* A token borrows its text from the source buffer: start/len delimit
   the raw slice (without quotes for strings). ival is set for T_INT.
   line/col are 1-based; columns count bytes. */
typedef struct {
  TokKind kind;
  const unsigned char *start;
  size_t len;
  int line;
  int col;
  int64_t ival;
} Tok;

/* Lexer state over one source buffer. After failed becomes true, the
   err_* fields describe the first error and lex_next keeps returning
   T_INVALID. */
typedef struct {
  const unsigned char *src;
  size_t len;
  size_t pos;
  int line;
  int col;
  int failed;
  int err_line;
  int err_col;
  const char *err_code;
  const char *err_msg;
} Lexer;

/* Contract: prepares a lexer over src[0..len). No failure modes. */
void lex_init(Lexer *lx, const unsigned char *src, size_t len);

/* Contract: returns the next token. On lex error returns T_INVALID
   with err_code/err_msg set (E2000 invalid UTF-8, E2001 lone CR,
   E2002 bad escape, E2003 integer range, E2004 unexpected character,
   E2005 unterminated string). */
Tok lex_next(Lexer *lx);

/* Contract: validates a whole buffer as strict UTF-8 (no overlongs,
   surrogates, or codepoints above U+10FFFF). Shared with the
   evaluator so both layers accept exactly the same bytes. */
int lex_valid_utf8(const unsigned char *p, size_t n);

#endif
