# Vow Reference Implementation (Slices 1–3 plus ABI slice:
Lexer, Parser, Checker, Evaluator, Test Runner, emit-dylib)

Status: delivered for review. Written in the conservative C subset
(`docs/c-subset.md`). Slices 1–3 cover lexing, parsing, checking,
evaluation, and test running: `vow check`, `vow run`, and
`vow test` are all implemented. The binary is now named `vow`
(renamed from `vow0`; the name arrives with the full core surface).
The ABI slice adds `vow build --emit-dylib`, which links a checked
entry plus the whole interpreter into a shared library
(`docs/abi-spec.md`, LOCKED).

## Layout

- `diag.h`, `diag.c` — catalog-coded diagnostics on stderr.
- `lex.h`, `lex.c` — lexer for the stage-0 token set.
- `ast.h`, `ast.c` — arena allocator and AST nodes.
- `parse.h`, `parse.c` — recursive descent parser for the stage-0
  grammar.
- `sha256.h`, `sha256.c` — SHA-256 for import-pin verification.
- `check.h`, `check.c` — the checker: scopes, import loading with
  hash and cycle verification, effect and name resolution.
- `eval.h`, `eval.c` — the evaluator: values, environments, calls,
  and the six capabilities.
- `run.h`, `run.c` — `vow run` (main execution) and `vow test`
  (collection, execution, reporting).
- `emit.h`, `emit.c` — `vow build --emit-dylib` (stub generation,
  abi.map, shared-library link) and the embedded runner the stub
  calls (`emit_run_embedded`, fed by `check_memory`).
- `platform.h`, `platform.c` — the single OS seam (spawn, temp
  files, directories, working directory via `plat_cwd`).
- `main.c` — command dispatch.
- `build.zig` — the build (a Zig toolchain suffices).
- `tests.sh` — slice acceptance: coverage tripwire, check assertions,
  run assertions, and one full-suite gate.

## Build

```
zig build
```

Requires a Zig toolchain (0.16.0 verified). Compiles the C core with
`-std=c11 -pedantic -Wall -Wextra -Werror -O2`, byte-identical flags
to the retired Makefile. `zig build test` builds and runs the full
suite. Native target by default; `-Dtarget=<triple>` selects a cross
target (POSIX-only surface still gates Windows — platform-module
follow-up, verified failing for `x86_64-windows-gnu` at
`sys/wait.h`).

## Commands

- `vow check <file.vow>` — lexes, parses, and checks one file
  plus everything it imports. Silent on success (exit 0). Prints
  `file:line:col: EXXXX message` and exits 2, 3, or 5 by diagnostic
  class (1 on usage errors).
- `vow run <file.vow>` — checks with main enforced, then executes
  `main`; its integer return is the process exit code.
- `vow test [dir]` — collects test blocks (default `tests/`) and
  reports `ok`/`not ok` lines plus a summary; exit 0 iff all pass.
- `vow build --emit-dylib <file.vow> [-o <out>]` — checks the
  entry (main required; failures propagate their class and write
  nothing), then generates `<sym>.abi.c`, `abi.map`, and
  `<sym>.ver` beside the output and links `<base>.so` with the
  locked flags plus `-fPIC -shared` and a version script derived
  from the map. Needs `cc` on PATH and the reference sources
  beside the binary, two levels up (zig-out layout), or under the
  working directory (`<cwd>/reference`); otherwise E1003.

## Slice Acceptance

```
zig build test
```

Part 0 runs the coverage tripwire (every diagnostic code has a
fixture or an explicit level-only mark); part 1+2 assert exit codes
and diagnostic lines over every fixture (plus `build` usage and
check-failure propagation); part 3 runs one full `vow test` of
the conformance tree from the repo root, which must exit 0; part
4 builds the three sample CLIs to temp dirs and asserts abi.map
content, exact dynamic exports (`nm`), dlopen returns equal to
`vow run`, and bit-identical rebuilds. Part 4 needs `cc`,
`python3`, `nm`, and `awk` on PATH and fails loudly by name
without them. Part 5 covers E5007 at implementation level
(POSIX-only, by catalog design): non-UTF-8 bytes through spawn
capture, `fs.read`, and `fs.list`, with byte fixtures minted by
system `printf` in a mktemp dir.

## Check Rules and Deferred Items

- The source root is the entry file's directory; `vow test [dir]`
  keeps the target directory as root. Escape is measured by depth
  arithmetic, so leading `..` in the invocation path behaves.
  Absolute entry paths stay absolute. Windows drive letters are a
  follow-up for the platform module, not handled in this slice.
- Sandbox order: root escape (E3006) is decided before any file
  outside the tree is read; hash verification (E3003) only covers
  files inside. par-04c therefore reports E3006 in practice (exit 3
  either way); genuine hash-mismatch coverage lives in par-04i.
- Calls to parameters, locals, and constants pass checking; the
  evaluator raises E5008 when the value is not callable, and must
  enforce R10 transitivity dynamically for such indirect calls,
  raising E5009 on violation.
- `main` presence (E3007) is wired behind a flag the `run` command
  sets; `vow check` accepts library files without `main`.
- `param_list := ident ("," ident)*` (bare identifiers, no type
  annotations) is recorded in the specification grammar (R11,
  ratified).
- Calling a non-function value raises E5008 at runtime (R12,
  decided); the parser and checker accept the shape.

## ABI Slice Notes (`vow build --emit-dylib`)

- No native codegen is claimed: the library reuses the whole
  interpreter. The stub embeds the checked entry bytes and the
  canonical absolute source root; the export runs `main` through
  `check_memory` plus eval and returns main's integer. Semantics
  are identical to `vow run` by construction (including imports,
  pins, and effects).
- Determinism scope: identical invocations on the same machine
  rebuild bit-identically (verified by `cmp` in the suite). The
  display path embeds as spelled, so different spellings of the
  same file are different (but each reproducible) artifacts.
- Export hiding uses a linker version script derived from
  `abi.map` (default visibility plus the script; `-fvisibility`
  games proved ineffective on the reference toolchain and source
  annotations stay banned). `nm -D --defined-only` must show
  exactly the mapped symbol.
- Failure codes reuse the catalog only: E1001 (bad build
  arguments), E1002 (entry missing, wins over output-directory
  problems), E1003 (reference sources, C compiler, or output
  directory missing), check failures propagate, E5006 (OS or
  compiler-step failures, with the captured compiler log on
  stderr). The two E1003 stretches need a ruling: accept, or
  issue a narrower code by joint decision.
- Follow-ups (not this slice): `.dylib`/`.dll` link flags on real
  macOS/Windows hardware, installer layout shipping the reference
  sources (or a PIC archive) so installed binaries can build, and
  whether per-CLI `abi.map` files should be committed next to the
  samples.

## Slice 3 Notes (Evaluator + Test Runner)

- Values are dynamically typed; every type mismatch is E5012, every
  resource breach E5006. Equality across different types is false.
  `&&` and `||` short-circuit. Falling off a function end returns 0
  (R21, pending ratification).
- Function values carry their home module; top-level constants
  evaluate lazily on first reference and memoize (never at import,
  per R9). R10 is re-verified at every user-function call (E5009);
  test bodies skip it (R2).
- `proc.spawn` captures through unlinked temp files (no pipe
  deadlock); environment and working directory are inherited
  (R3-interim). A declared-but-unrunnable command is E5010,
  signaled children map to 128 plus signal number.
- The value pool is bump-only (512 MiB, E5006 at exhaustion): safe
  but never reclaims; a production implementation needs collection.
- `vow test` skips `fail` directories (R17); unparseable collected
  files report `not ok` entries with exit 4. Paths echo the
  invocation spelling; the suite runs from the repo root per R3.
