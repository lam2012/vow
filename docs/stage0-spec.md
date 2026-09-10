# Vow Stage-0 Core Specification

Status: LOCKED. Signed by both design parties. Scope: the minimal core only — lexer, parser,
evaluator/VM, and test runner. Package manager, formatter, language server,
and registry are explicitly out of scope. The reference implementation is
written in a conservative C subset; cross-build orchestration uses the Zig
build system. This document specifies behavior, not implementation.

## 1. Goals

1. The core is small enough to audit in full.
2. Execution is deterministic: one source tree produces equivalent results on
   Windows, Linux, and macOS.
3. Everything a program can do to the outside world is declared and
   machine-checked.
4. The core can run its own build scripts and its own test runner, and
   nothing more.

Non-goals: user-facing tooling, performance optimization, language
expressiveness beyond build and test scripting.

## 2. Source Files

- Files are UTF-8 encoded and validated on input; invalid UTF-8 is a lex
  error.
- Line terminator is LF. A CRLF pair is accepted and treated as LF. A lone
  CR is a lex error.
- Strings are byte-oriented. Length and indexing operate on bytes.
- Line comments start with `#` and run to end of line. There are no block
  comments.
- Newlines are insignificant; blocks are delimited by braces.

## 3. Lexer Tokens

- Identifier: `[A-Za-z_][A-Za-z0-9_]*` (ASCII only at stage-0).
- Integer literal: `[0-9]+`, value range is signed 64-bit; a literal outside
  the range is a lex error.
- String literal: double-quoted with escapes `\\`, `\"`, `\n`, `\t` only.
  Any other escape is a lex error. There is no interpolation at stage-0.
- Keywords (closed set, reserved): `func`, `let`, `if`, `else`, `return`,
  `true`, `false`, `import`, `effect`, `test`, `assert`, `for`, `in`.
- Symbols: `(` `)` `{` `}` `[` `]` `,` `:` `=` `==` `!=` `<` `<=` `>` `>=`
  `+` `-` `*` `/` `%` `!` `&&` `||` `.`.
- There are no floating-point literals, and no other numeric bases, at
  stage-0.

## 4. Grammar

```ebnf
program       := decl*
decl          := import_decl | effect_decl | const_def | func_def | test_def
import_decl := "import" string "as" ident "hash" string
effect_decl := "effect" ident "(" param_list? ")"
param_list  := ident ("," ident)*
const_def     := "let" ident "=" pure_expr
func_def      := "func" ident "(" param_list? ")" effect_clause? block
effect_clause := "effect" "[" effect_entry ("," effect_entry)* "]"
effect_entry  := dotted_ident ("[" string ("," string)* "]")?
test_def      := "test" string block
block         := "{" stmt* "}"
stmt          := "let" ident "=" expr
               | "assert" expr ("," string)?
               | expr
               | "if" expr block ("else" block)?
               | "for" ident "in" expr block
               | "return" expr?
expr          := or_expr
or_expr       := and_expr ("||" and_expr)*
and_expr      := eq_expr ("&&" eq_expr)*
eq_expr       := cmp_expr (("==" | "!=") cmp_expr)*
cmp_expr      := add_expr (("<" | "<=" | ">" | ">=") add_expr)*
add_expr      := mul_expr (("+" | "-") mul_expr)*
mul_expr      := unary_expr (("*" | "/" | "%") unary_expr)*
unary_expr    := ("!" | "-") unary_expr | postfix_expr
postfix_expr  := primary (call_suffix | index_suffix)*
call_suffix   := "(" arg_list? ")"
index_suffix  := "[" expr "]"
primary       := callee | int | string | "true" | "false"
               | list_lit | map_lit | "(" expr ")"
callee        := ident ("." ident)*
dotted_ident  := ident ("." ident)*
list_lit      := "[" (expr ("," expr)*)? "]"
map_lit       := "{" (string ":" expr ("," string ":" expr)*)? "}"
pure_expr     := expr without calls, without effect operations
```

- Operator precedence is fixed by the hierarchy above. `!` and unary `-`
  bind tightest. Calls and indexing associate left.
- `for x in e` iterates over a list value in order. There is no `while`
  loop and no `break` at stage-0; every loop is bounded by a finite list.
- A `{`-led map literal in statement position parses as an expression
  statement; blocks appear only after `func`, `if`, `else`, `for`, and
  `test` headers, so the grammar is unambiguous.
- A function with no `effect` clause is pure: its body must contain no
  effect operations. Violation is a check error.
- Top-level `let` initializers must be pure constant expressions. Module
  import performs no code execution.
- Duplicate map keys in a literal are a check error.

## 5. Values and Operators

- Runtime values: 64-bit signed integer, UTF-8 string, boolean, list of
  values, map from string to value.
- Map keys are strings only at stage-0. Map equality is order-insensitive;
  iteration order is always sorted by key (Section 7).
- Arithmetic operators apply to integers. Integer overflow is a runtime
  failure, not wrapping.
- Comparison operators apply to integers and strings. Equality applies to
  all values.
- Division or modulo by zero is a runtime failure.
- Indexing: a list accepts an integer index (0-based; out of range is a
  runtime failure); a map accepts a string key (a missing key is a
  runtime failure); a string accepts an integer byte offset (out of range
  is a runtime failure).
- There are no exceptions and no catch mechanism at stage-0. A runtime
  failure aborts the program with a diagnostic and a nonzero exit code.
- Calling a value that is not a function is a runtime failure (R12).
  There is no static type to reject it earlier.
- A function body that falls off the end without `return` yields 0
  (R21).
- Applying any other operator to values outside its domain (non-integer
  arithmetic, non-boolean conditions, non-list iteration, wrong-kind
  indices) is a runtime type error, E5012. Equality across different
  types is false, never an error.

## 6. Effects and Dependencies: Static Rules

- Effect operations form a closed set (Section 7). Calling one inside a
  function that does not declare it in its `effect` clause is a check
  error. Undeclared effect names in an `effect` clause are a check error.
- Effects are transitive through calls: a function may only call
   functions whose declared effect set is a subset of its own clause. A
   call that would widen the set is a check error. (Ratified delta R10,
   joint decision of both parties.) Calls through parameters and
   locals, whose callees are only known at runtime, re-verify the
   subset at call time; a violation there is E5009.
- Effect entries carry their declared scope as string-literal arguments,
  for example `effect[fs.read["src/"], proc.spawn["zig", "cc"]]`.
  Non-literal arguments are rejected at parse time (E2006). The
  declared strings are part of the auditable surface.
- Undeclared capabilities do not exist: there is no ambient file system,
  environment, clock, or network access. A program reaches the outside
  world only through declared effects.
- Imports are explicit and pinned: every `import` carries the expected
  content hash of the target module. A hash mismatch is a check error
  before any code runs.
- Import paths are relative to the importing file and normalized
  lexically. A path escaping above the source root is a check error.
  The source root is the entry file's directory for `vow run`, and the
  target directory for `vow test`. A module's identity is its
  root-relative path plus its pinned hash.
- Importing a module executes no code. Cyclic imports are a check error.
- Using a name that no declaration binds is a check error (R13).

## 7. Evaluator Capability Allowlist

The evaluator exposes exactly these capabilities, and nothing else:

| Capability     | Declaration shape and allowed operation                            |
| -------------- | ------------------------------------------------------------------ |
| `fs.read`      | `fs.read["dir/", ...]` — read files under declared directory       |
|                | paths. A read outside every declared path is a runtime failure.    |
| `fs.write`     | `fs.write["out/", ...]` — write files under declared output        |
|                | directories. A write outside every declared path is a runtime      |
|                | failure. (Ratified delta, see Section 12 item 6.)                  |
| `fs.list`      | `fs.list["dir/", ...]` — list a declared directory. Results are    |
|                | sorted byte-wise. (Ratified delta, see Section 12 item 6.)         |
| `proc.spawn`   | `proc.spawn["cmd", ...]` — spawn a declared command. Argv           |
|                | elements are string literals or prefix patterns: a literal ending  |
|                | in `*` matches any continuation, all other elements match exactly. |
|                | Declared extra argv beyond the patterns stays free. Until workdir  |
|                | and environment declaration syntax lands (R3 debt), processes      |
|                | inherit the harness working directory and environment. Anything    |
|                | else is a runtime failure.                                         |
| `hash.sha256`  | Pure content hashing. Takes no declaration arguments.              |
| `lock.check`   | Pure verification of lockfile entries against content hashes,      |
|                | returning true only when every pin matches. Takes no declaration   |
|                | arguments.                                                         |

- Map iteration order is sorted by key. Directory listings are sorted.
  No locale-dependent ordering is used anywhere.
- Relative fs paths anchor at the source root (the entry file's
  directory for `vow run`, the target directory for `vow test`).
  A read, write, or listing whose resolved path falls outside every
  declared directory is a runtime failure; a missing target inside
  the declared scope is E5010.
- There is no clock, no randomness source, no threading, and no network
  operation at stage-0.
- The evaluator enforces fixed default resource limits: 10^8 evaluation
  steps, 512 MiB live memory, 1024 frames of recursion depth, 64 MiB per
  single file read. Exceeding a limit is a runtime failure. Limits may
  be raised through explicit CLI flags; raising a limit never changes an
  output value, only whether evaluation aborts.
- Native FFI does not exist at stage-0. Contact with non-Vow code happens
   through `proc.spawn` only; direct library calls arrive with later ABI
   work.
- `proc.spawn` returns a map `{exit, stdout, stderr}`: `exit` is the
   process exit code as an integer; `stdout` and `stderr` are the captured
   outputs as byte-oriented strings. Process output that is not valid
   UTF-8 is a runtime failure, because core strings are UTF-8-validated
   (Section 2) and stage-0 has no byte-buffer type. (Ratified delta,
   joint decision of both parties.)
- Capability call shapes: every capability operation is invoked with a
   single list argument. `fs.read([path])` returns the file content as a
   string. `fs.write([path, content])` returns bytes written as an
   integer. `fs.list([dir])` returns entry names as a list of strings.
   `proc.spawn([cmd, args...])` returns the result map. `hash.sha256` and
   `lock.check` are pure and take their inputs as a list as well.
   (Ratified delta, joint decision of both parties.)

## 8. Test-Runner Protocol

- The runner collects `test` blocks from files under a target directory
  (default `tests/`), in byte-sorted path order, then definition order.
  Files and directories whose names start with `_` are skipped, as are
  directories named exactly `fail` (negative cases run exclusively
  through driver assertions, R17). A collected file that fails to
  lex, parse, or check reports one `not ok` entry carrying its code.
- `&&` and `||` short-circuit: the right side runs only when needed.
- `assert expr` passes when the value is boolean true, and fails
  otherwise. `assert expr, string` attaches a message to the failure.
  `assert` is a statement and may appear in any block, not only in
   `test` blocks.
- Test blocks may call functions that declare effects; each call is
   checked against the callee's `effect` clause. (Ratified delta, joint
   decision of both parties.)
- Per-test output lines: `ok <name>` or `not ok <name> <message>`,
  each with a `file:line` reference.
- The summary line reports counts of passed and failed tests.
- Exit code is 0 if and only if every collected test passes. The runner
  performs no network access.

## 9. Commands and Diagnostics

- `vow run <file.vow>` executes the `main` function of the file. `main`
  is declared as `func main() effect[...] { ... }`: it takes no
  parameters, returns an integer used as the process exit code
  (a non-integer return is E5011; the integer is truncated to 8 bits
  as in C), and its `effect` clause follows
  the same declaration rules as any function
  (omitted means pure). A file without `main` is a check error under
  `vow run`.
- `vow test [dir]` runs the test protocol of Section 8.
- Diagnostics use the format `file:line:col: EXXXX message` on stderr.
- Exit codes: 1 for usage errors, 2 for lex/parse errors, 3 for check
  errors, 4 for test failures, 5 for runtime failures including effect
  violations and resource-limit breaches.

## 10. Reproducibility

- Reproducibility is verified by hash: the conformance procedure rebuilds
  the same tree on each OS and compares content hashes of declared
  outputs, excluding OS-native packaging formats.
- Timestamps are never embedded in declared outputs. Where an output
  format requires a timestamp field, the producer writes a fixed
  constant documented with the format.
- Version stamping uses content hashes plus a version string declared in
  source. There is no clock to read the time from (Section 12 item 1).

## 11. Explicitly Out of Scope

Package manager, formatter, language server, package registry,
floating-point numbers, threading, network operations, clock and
randomness sources, native FFI, exceptions, and generics are all out of
scope for stage-0. None of them may be added implicitly through
implementation convenience.

## 12. Resolutions of Open Items

1. Clock access: resolved as no clock. Version stamping uses content
   hashes plus a declared version string. No logical clock is added at
   stage-0; the allowlist discipline forbids surplus capabilities.
2. Integer overflow: resolved as a runtime failure. Silent wrapping is a
   historic bug source and contradicts explicitness. Checked-wrapping
   operators may be proposed later for performance work, which is a
   stage-0 non-goal.
3. Default resource limits: resolved as 10^8 steps, 512 MiB live memory,
   1024 recursion frames, 64 MiB per file read, raisable through explicit
   CLI flags per Section 7.
4. `proc.spawn` declaration schema: resolved as literal command, literal
   or prefix-pattern argv, declared-only environment, declared working
   directory, per Section 7.
5. Module file layout: resolved as one module per `.vow` file, importing
   file-relative normalized paths, no escape above the source root,
   module identity as root-relative path plus pinned hash, per
   Section 6.
6. Allowlist delta (ratified, pre-approved): `fs.write` and `fs.list`
   were not in the charter's original four-item allowlist (read file,
   spawn, hash, lockfile). Both are necessary — a build script that
    cannot write outputs is useless, and directory listing must be sorted
    to be decidable. They are recorded here so the audit history is
    clean.

## 13. Sign-off

- Party A: signed.
- Party B: signed.
