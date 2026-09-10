# Vow Conservative C Subset

Status: LOCKED. Signed by both design parties. This document closes the definition of the C
subset in which the reference implementation is written. Anything not
listed here is forbidden until a joint decision adds it. The goal is a
codebase that two reviewers can audit in full: bounded resources,
no hidden control flow, no platform-dependent behavior.

## 1. Language Standard and Flags

- ISO C11, strict: `-std=c11 -pedantic`.
- Warning set, all errors: `-Wall -Wextra -Werror`.
- No compiler extensions (`__attribute__`, nested functions, statement
  expressions, `typeof`). No `#pragma`.
- Optimization level does not change observable behavior; the audited
  build uses `-O2`.

## 2. Allowed Headers

`stdint.h`, `stdbool.h`, `stddef.h`, `string.h` (only `memcpy`, `memmove`,
`memcmp`, `memset`, `strlen` on validated input — see Section 5),
`stdio.h` (only `fopen` in `"rb"`/`"wb"` mode, `fread`, `fwrite`,
`fclose`, `fprintf(stderr, ...)` with a static format string,
`putc`), `stdlib.h` (only `exit`, `EXIT_SUCCESS`, `EXIT_FAILURE`),
`limits.h`, `errno.h` (read-only, for file errors), `inttypes.h`
(only `PRId64`), `platform.h` (the single OS seam).

The POSIX surface (`unistd.h` for `fork`/`execvp`/`dup2`/`close`/
`read`/`write`/`_exit`/`unlink`/`getcwd`, `sys/wait.h`, `dirent.h`,
`stdlib.h`'s `mkstemp`) lives in exactly one translation unit:
`platform.c`. No other file includes a platform header. Windows
equivalents arrive inside the same seam (follow-up); no other
platform headers until then. `getcwd` enters through `plat_cwd`,
which pins the build working directory into dylib stubs.

Everything else needs a joint decision. In particular slice 1 uses no
`malloc.h`, no `unistd.h`, no `windows.h`, no `time.h`, no `locale.h`.

## 3. Memory: Arena Only

- `malloc`, `calloc`, `realloc`, and `free` are forbidden.
- All dynamic memory comes from bump arenas with fixed capacity,
  declared at creation. Arena exhaustion is a fatal diagnostic, never
  silent truncation.
- No ownership transfer: arenas outlive everything they allocate, and
  are released as a whole at a single, visible point.
- Maximum recursion depth of the parser is a named constant
  (`PARSE_MAX_DEPTH`); exceeding it is a parse error, not a stack
  overflow.

## 4. Forbidden Constructs

- Function pointers (no hidden call targets).
- `goto` (single-entry, single-exit control flow only).
- `setjmp` / `longjmp` (errors return through the call chain with
  explicit codes).
- Variable-length arrays.
- `float` and `double` (no floating point at stage-0, in any layer).
- Threads, atomics, signals, `volatile` for synchronization.
- `system`, `popen`, `exec*`, `dlopen` (process spawning arrives with
  the evaluator slice behind a documented narrow wrapper).
- `getenv`, `setenv` (no ambient environment reads).
- `time`, `clock`, `rand` and friends (no clock or randomness sources).
- `locale.h` and every locale-dependent function (`strcoll`, `atoi`,
  `strtod`); parsing is manual and byte-oriented.
- `strcpy`, `strcat`, `sprintf` (unbounded copies). `fprintf` to
  stderr with a static format string is the single allowed variadic
  use, for diagnostics only; `snprintf` is allowed only into a fixed
  buffer whose size is asserted at the call site, and only for
  diagnostics.
- Bit-fields and `union` type punning (strict aliasing stays intact).

## 5. Integers and Strings

- Sizes and counts are `size_t`. Values are `int64_t`, `uint64_t`,
  `bool`, or byte slices (`const unsigned char *` plus length).
- All integer arithmetic that can overflow is written through checked
  helpers introduced with the first slice that needs them; an
  overflow path is always an explicit diagnostic, never wrapping.
- Strings are length-delimited everywhere. NUL termination is never
  relied upon for parsed content.
- UTF-8 is validated strictly (reject overlongs, surrogates, and
  codepoints above U+10FFFF) at the lexer boundary, once.

## 6. Input and Output

- Files open in binary mode. The lexer normalizes CRLF to LF and
  rejects a lone CR.
- Diagnostics go to stderr in the catalog format
  `file:line:col: EXXXX message`. Columns count bytes from 1.
- Exit codes follow the stage-0 specification, Section 9. Slice 1
  implements classes 0, 1, and 2; classes 3–5 arrive with later slices
  and are already reserved.

## 7. Portability

- No platform `#ifdef` anywhere. Path handling outside the seam is
  byte-oriented; separators are never interpreted by the lexer or
  parser.
- The single platform module is `platform.h`/`platform.c` (POSIX
  implementation). Every operating-system call lives behind it; the
  rest of the codebase includes no platform headers. Anything
  OS-specific outside that module is a defect. Windows follow-ups are
  documented per function inside the seam (D033 style).

## 8. Style

- One module per concern: `diag`, `lex`, `ast`, `parse`, `main`,
  `platform`, `check`, `sha256`, `eval`, `run`, `emit`.
- Public names carry the module prefix (`lex_`, `parse_`, `diag_`,
  `plat_`, `check_`, `sha256_`, `ev_`, `run_`, `emit_`).
  The shared vocabulary is listed here exhaustively and needs no
  prefix: `Arena`, `arena_init`, `arena_alloc`, `arena_align_up`,
  `Slice`, `Tok`, `TokKind`, `Lexer`, `Parser`, `Expr`, `ExprKind`,
  `Stmt`, `StmtKind`, `Decl`, `DeclKind`, `EffectEntry`, `Checker`,
  `Val`, `Env`, `Ev`. Anything else takes a module prefix.
- Every function documents its contract in one comment block: inputs,
  outputs, and all failure modes.
- No dead code: every function and constant is reachable from `main`
  or from the test driver.

## 9. Audit Checklist

A slice is reviewable when all of the following hold:

- [x] It builds clean under the Section 1 flags with both `cc` and a
      second C11 compiler when available. (Second compiler: clang
      22.1.8, Linux x86-64 host, verified 2026-09-10: plain and UBSan
      builds run the full suite green; the address-sanitizer build is
      green except eff-05, where fatter C frames exhaust the 8 MB
      native stack before the 1024-level Vow guard trips — the same
      binary returns E5006 with an unbounded stack. Sanitizer-only
      divergence; release builds unaffected. Countersigned A+B
      2026-09-10.)
- [ ] `grep -rn` for every Section 4 item returns nothing.
- [ ] The slice test script passes on all checked-in fixtures.
- [ ] Every new diagnostic carries a catalog code from
      `docs/error-codes.md`.
- [ ] No slice exceeds its stated scope (lexer+parser: no checking, no
      evaluation, no file writes).

## 10. Sign-off

- Party A: signed.
- Party B: signed.
