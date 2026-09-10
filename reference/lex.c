#include "lex.h"

/* Byte classes. Only ASCII matters outside strings; the subset bans
   locale-dependent classification. */
static int is_alpha_(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_digit_(unsigned char c) {
  return c >= '0' && c <= '9';
}

static int is_alnum_(unsigned char c) {
  return is_alpha_(c) || is_digit_(c);
}

/* Strict UTF-8 sequence length at p with n bytes available, or 0 when
   invalid. Rejects overlongs, surrogates, and codepoints above
   U+10FFFF. */
static size_t utf8_seq_len(const unsigned char *p, size_t n) {
  unsigned char c;
  size_t len;
  uint32_t cp;
  uint32_t min;
  size_t i;
  if (n == 0) {
    return 0;
  }
  c = p[0];
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    cp = (uint32_t)(c & 0x1F);
    min = 0x80;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    cp = (uint32_t)(c & 0x0F);
    min = 0x800;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    cp = (uint32_t)(c & 0x07);
    min = 0x10000;
  } else {
    return 0;
  }
  if (len > n) {
    return 0;
  }
  for (i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0;
  }
  return len;
}

int lex_valid_utf8(const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t len = utf8_seq_len(p + i, n - i);
    if (len == 0) {
      return 0;
    }
    i += len;
  }
  return 1;
}

typedef struct {
  const char *word;
  TokKind kind;
} Keyword;
/* Closed keyword set from the stage-0 specification, Section 3. */
static const Keyword kKeywords[] = {
  {"assert", T_ASSERT},
  {"effect", T_EFFECT},
  {"else", T_ELSE},
  {"false", T_FALSE},
  {"for", T_FOR},
  {"func", T_FUNC},
  {"if", T_IF},
  {"import", T_IMPORT},
  {"in", T_IN},
  {"let", T_LET},
  {"return", T_RETURN},
  {"test", T_TEST},
  {"true", T_TRUE},
};
static const size_t kKeywordCount = sizeof(kKeywords) / sizeof(kKeywords[0]);

void lex_init(Lexer *lx, const unsigned char *src, size_t len) {  lx->src = src;
  lx->len = len;
  lx->pos = 0;
  lx->line = 1;
  lx->col = 1;
  lx->failed = 0;
  lx->err_line = 1;
  lx->err_col = 1;
  lx->err_code = "";
  lx->err_msg = "";
}

/* Records the first error; later calls keep returning invalid. */
static Tok lex_fail(Lexer *lx, const char *code, const char *msg) {
  Tok t;
  lx->failed = 1;
  lx->err_line = lx->line;
  lx->err_col = lx->col;
  lx->err_code = code;
  lx->err_msg = msg;
  t.kind = T_INVALID;
  t.start = lx->src + lx->pos;
  t.len = 0;
  t.line = lx->line;
  t.col = lx->col;
  t.ival = 0;
  return t;
}

static Tok lex_at(Lexer *lx, TokKind kind, const unsigned char *start,
                  size_t len, int line, int col) {
  Tok t;
  (void)lx;
  t.kind = kind;
  t.start = start;
  t.len = len;
  t.line = line;
  t.col = col;
  t.ival = 0;
  return t;
}

/* Consumes one newline; CRLF counts once, lone CR is E2001. */
static int lex_newline(Lexer *lx, Tok *bad) {
  if (lx->src[lx->pos] == '\r') {
    if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '\n') {
      lx->pos += 2;
    } else {
      *bad = lex_fail(lx, "E2001", "lone carriage return");
      return 0;
    }
  } else {
    lx->pos += 1;
  }
  lx->line += 1;
  lx->col = 1;
  return 1;
}

Tok lex_next(Lexer *lx) {
  unsigned char c;
  Tok bad;
  if (lx->failed) {
    return lex_fail(lx, lx->err_code, lx->err_msg);
  }
  /* Skip whitespace and line comments. */
  for (;;) {
    if (lx->pos >= lx->len) {
      return lex_at(lx, T_EOF, lx->src + lx->len, 0, lx->line, lx->col);
    }
    c = lx->src[lx->pos];
    if (c == ' ' || c == '\t') {
      lx->pos += 1;
      lx->col += 1;
    } else if (c == '\n' || c == '\r') {
      if (!lex_newline(lx, &bad)) {
        return bad;
      }
    } else if (c == '#') {
      while (lx->pos < lx->len && lx->src[lx->pos] != '\n' &&
             lx->src[lx->pos] != '\r') {
        lx->pos += 1;
        lx->col += 1;
      }
    } else {
      break;
    }
  }
  {
    const unsigned char *start = lx->src + lx->pos;
    int line = lx->line;
    int col = lx->col;
    size_t i;
    c = lx->src[lx->pos];
    /* Identifier or keyword: ASCII only at stage-0. */
    if (is_alpha_(c)) {
      size_t len = 0;
      while (lx->pos + len < lx->len && is_alnum_(lx->src[lx->pos + len])) {
        len += 1;
      }
      lx->pos += len;
      lx->col += (int)len;
      for (i = 0; i < kKeywordCount; i++) {
        const char *w = kKeywords[i].word;
        size_t n = 0;
        size_t k;
        int same;
        while (w[n] != '\0') {
          n += 1;
        }
        if (n != len) {
          continue;
        }
        same = 1;
        for (k = 0; k < n; k++) {
          if (start[k] != (unsigned char)w[k]) {
            same = 0;
            break;
          }
        }
        if (same) {
          return lex_at(lx, kKeywords[i].kind, start, len, line, col);
        }
      }
      return lex_at(lx, T_IDENT, start, len, line, col);
    }
    /* Integer literal with range check against int64. */
    if (is_digit_(c)) {
      uint64_t v = 0;
      uint64_t d;
      size_t len = 0;
      while (lx->pos < lx->len && is_digit_(lx->src[lx->pos])) {
        d = (uint64_t)(lx->src[lx->pos] - '0');
        /* Exact overflow check for v*10+d against int64 max. */
        if (v > ((uint64_t)9223372036854775807ULL - d) / 10u) {
          while (lx->pos < lx->len && is_digit_(lx->src[lx->pos])) {
            lx->pos += 1;
            lx->col += 1;
            len += 1;
          }
          return lex_fail(lx, "E2003", "integer literal out of range");
        }
        v = v * 10u + d;
        lx->pos += 1;
        lx->col += 1;
        len += 1;
      }
      {
        Tok t = lex_at(lx, T_INT, start, len, line, col);
        t.ival = (int64_t)v;
        return t;
      }
    }
    /* String literal: escapes validated, UTF-8 validated, raw bytes kept. */
    if (c == '"') {
      const unsigned char *inner;
      lx->pos += 1;
      lx->col += 1;
      inner = lx->src + lx->pos;
      for (;;) {
        unsigned char s;
        size_t seqlen;
        if (lx->pos >= lx->len) {
          return lex_fail(lx, "E2005", "unterminated string literal");
        }
        s = lx->src[lx->pos];
        if (s == '"') {
          Tok t = lex_at(lx, T_STR, inner, lx->src + lx->pos - inner,
                         line, col);
          lx->pos += 1;
          lx->col += 1;
          return t;
        }
        if (s == '\\') {
          unsigned char e;
          if (lx->pos + 1 >= lx->len) {
            return lex_fail(lx, "E2005", "unterminated string literal");
          }
          e = lx->src[lx->pos + 1];
          if (e != '\\' && e != '"' && e != 'n' && e != 't') {
            return lex_fail(lx, "E2002", "unknown string escape");
          }
          lx->pos += 2;
          lx->col += 2;
          continue;
        }
        if (s == '\n' || s == '\r') {
          if (!lex_newline(lx, &bad)) {
            return bad;
          }
          continue;
        }
        if (s < 0x80) {
          lx->pos += 1;
          lx->col += 1;
          continue;
        }
        seqlen = utf8_seq_len(lx->src + lx->pos, lx->len - lx->pos);
        if (seqlen == 0) {
          return lex_fail(lx, "E2000", "invalid UTF-8 input");
        }
        lx->pos += seqlen;
        lx->col += (int)seqlen;
      }
    }
    /* Two-character symbols first. */
    if (lx->pos + 1 < lx->len) {
      unsigned char d = lx->src[lx->pos + 1];
      TokKind two = T_INVALID;
      if (c == '=' && d == '=') {
        two = T_EQ;
      } else if (c == '!' && d == '=') {
        two = T_NEQ;
      } else if (c == '<' && d == '=') {
        two = T_LTE;
      } else if (c == '>' && d == '=') {
        two = T_GTE;
      } else if (c == '&' && d == '&') {
        two = T_AMPAMP;
      } else if (c == '|' && d == '|') {
        two = T_PIPEPIPE;
      }
      if (two != T_INVALID) {
        Tok t = lex_at(lx, two, start, 2, line, col);
        lx->pos += 2;
        lx->col += 2;
        return t;
      }
    }
    /* Single-character symbols. */
    {
      TokKind one = T_INVALID;
      switch (c) {
      case '(': one = T_LPAREN; break;
      case ')': one = T_RPAREN; break;
      case '{': one = T_LBRACE; break;
      case '}': one = T_RBRACE; break;
      case '[': one = T_LBRACK; break;
      case ']': one = T_RBRACK; break;
      case ',': one = T_COMMA; break;
      case ':': one = T_COLON; break;
      case '=': one = T_ASSIGN; break;
      case '<': one = T_LT; break;
      case '>': one = T_GT; break;
      case '+': one = T_PLUS; break;
      case '-': one = T_MINUS; break;
      case '*': one = T_STAR; break;
      case '/': one = T_SLASH; break;
      case '%': one = T_PERCENT; break;
      case '!': one = T_BANG; break;
      case '.': one = T_DOT; break;
      default: break;
      }
      if (one != T_INVALID) {
        Tok t = lex_at(lx, one, start, 1, line, col);
        lx->pos += 1;
        lx->col += 1;
        return t;
      }
    }
    /* Non-ASCII bytes: malformed UTF-8 is E2000, well-formed but
       unlexable input is E2004. */
    if (c >= 0x80) {
      size_t seqlen = utf8_seq_len(lx->src + lx->pos, lx->len - lx->pos);
      if (seqlen == 0) {
        return lex_fail(lx, "E2000", "invalid UTF-8 input");
      }
      lx->pos += seqlen;
      lx->col += (int)seqlen;
      return lex_fail(lx, "E2004", "unexpected character");
    }
    lx->pos += 1;
    lx->col += 1;
    return lex_fail(lx, "E2004", "unexpected character");
  }
}
