# Vow Conformance Suite Skeleton

Status: LOCKED. Signed by both design parties. This document defines the layout, harness command,
reproducibility procedure, and first test vectors of the conformance suite.
It covers the first two charter success metrics (one-command green suite on
three OSes; hash-checked reproducible artifacts). The third metric (three
sample CLIs installed offline) runs on a separate track. Normative language
behavior stays in the stage-0 specification; this document only organizes
how compliance is checked.

## 1. Layout

```
conformance/
├── vow/
│   ├── lexer/     # LEX-xxx fixtures
│   ├── grammar/   # PAR-xxx fixtures
│   ├── effects/   # EFF-xxx fixtures
│   └── runner/    # RUN-xxx fixtures
├── expected/      # expected stdout, exit codes, and content hashes
├── lockfiles/    # pinned hashes for fixture imports
└── driver.vow    # negative-case driver (see Section 3)
```

- One fixture per vector: `<AREA>-<NN>-<short-name>.vow`.
- Fixtures are split by expectation: `pass/` (exit 0, exact stdout match)
  and `fail/` (expected exit code 2, 3, 4, or 5 per stage-0 Section 9, plus
  expected diagnostic prefix `file:line:col: EXXXX`).
- Expected outputs live beside fixtures under `expected/` with mirrored
  paths. Where an expectation legitimately differs per OS, an overlay file
  `expected/<os>/...` replaces the neutral one; overlays require a comment
  citing the reason. No silent per-OS divergence.

## 2. Harness Command

The whole suite runs with one command:

```
vow test conformance/
```

- Positive vectors are plain `test` blocks asserting on fixture behavior.
- Negative vectors (expected check errors, runtime failures) go through
  `driver.vow`: it invokes the `vow` binary on `fail/` fixtures with
  `proc.spawn`, captures results, and exposes them to `test` blocks that
  `assert` on exit codes and diagnostic prefixes.
- The driver only uses allowlisted capabilities (`proc.spawn` on the
  declared `vow` binary, `hash.sha256` for output comparison, `fs.read` on
  fixture directories). The suite is therefore runnable by the core it
  tests. Bootstrapping order: the reference C implementation runs the
  suite first; the suite then guards every later implementation including
  the self-hosted one.

## 3. Reproducibility Procedure

1. On Windows, Linux, and macOS, run `vow test conformance/` on the same
   source tree in an offline environment.
2. Collect declared outputs (suite stdout, summary counts, fixture
   artifacts under test).
3. Compare SHA-256 hashes of the outputs across the three runs. They must
   match, excluding OS-native packaging formats.
4. Rules that keep the procedure honest: byte-sorted collection order,
   sorted map iteration and directory listings, no timestamps in declared
   outputs (fixed constants where a format demands one), no network access
   at any step.

## 4. First Vectors

Each vector names its stage-0 section and its expected exit code.

| ID | Spec ref | Case | Expect |
| -- | -------- | ---- | ------ |
| LEX-01 | §3 | all token kinds lexed | 0 |
| LEX-02 | §2–3 | invalid UTF-8, lone CR, bad escape, out-of-range integer | 2 |
| PAR-01 | §4 | operator precedence and associativity | 0 |
| PAR-02 | §4 | `for` over list in order; no `while`, no `break` | 0 / 2 |
| PAR-03 | §4 | map literal vs block disambiguation in statement position | 0 |
| PAR-04 | §4, §6 | duplicate map keys, cyclic imports, hash mismatch, undeclared effect name | 3 |
| PAR-05 | §4 | import executes no code | 0 |
| EFF-01 | §6–7 | undeclared effect call is a check error | 3 |
| EFF-02 | §7 | read/write outside declared paths, undeclared spawn, tainted environment | 5 |
| EFF-03 | §7 | sorted listing, sorted map iteration, byte-oriented strings | 0 |
| EFF-04 | §7 | no clock, randomness, threading, network, or FFI surface | 3 |
| EFF-05 | §7 | resource limits trip on runaway evaluation | 5 |
| RUN-01 | §8 | `ok` / `not ok` lines with `file:line`, summary counts | 0 |
| RUN-02 | §8 | exit 0 iff every collected test passes; message attach | 4 on failure |
| RUN-03 | §8–9 | `main` signature rules; missing `main` under `vow run` | 3 |
| REP-01 | §10 | same-tree hash equality across the three OS runs | procedure |

## 5. Out of Scope

Performance benchmarks, fuzzing, dynamic-library ABI checks (second gate
of phase 2), and sample-CLI installation tests are out of scope for this
skeleton. None may be smuggled in as fixture convenience.

## 6. Resolutions

1. `proc.spawn` result shape: ratified as a map `{exit, stdout, stderr}`
   (`exit` integer; `stdout`/`stderr` byte-oriented strings; non-UTF-8
   process output is a runtime failure). Recorded as language semantics in
   the stage-0 specification, Section 7.
2. Overlay budget: ceiling is zero at skeleton stage; each new per-OS
   overlay needs a joint exception.
3. Fixture lockfiles: the reference implementation mints them; the suite
   verifies them, in bootstrap order (C reference runs first).
4. Capability call shapes (R1): every capability operation takes a single
   list argument — `fs.read([p])`, `fs.write([p, content])`,
   `fs.list([d])`, `proc.spawn([cmd, args...])`, `hash.sha256([s])`,
   `lock.check([lockfile])`. Recorded as language semantics in the
   stage-0 specification, Section 7.
5. Test-block transparency (R2): test blocks may call functions that
   declare effects; the call is checked against the callee's clause.
   Recorded in the stage-0 specification, Section 8.
6. Spawn working directory (R3): a spawned process inherits the harness
   working directory. Follow-up debt: declaration syntax for an explicit
   working directory is still missing from the specification.
7. Collection (R4–R5): collection is recursive; files and directories
   whose names start with `_` are skipped.
8. Runner output format (R6): the summary line is
   `pass: <n> fail: <m>`; `file:line` references are root-relative with
   `/` separators, which is required for hash equality across OSes.
9. Diagnostic codes (R7): the driver asserts exit codes only for now; the
   E-code catalog is mandatory before the C reference is written
   (drafted separately).
10. Check-error order (R8): conformance is exit-code-only; when several
    check errors coincide, detection order is unspecified at stage-0.
11. Import binding (R9): importing binds without evaluating; the collector
    does not evaluate top-level initializers of unrelated files.
12. Effect transitivity (R10): a callee's effect set must be a subset of
    the caller's clause; violations are E3000. Test blocks stay
    transparent per item 5.

## 7. Sign-off

- Party A: signed.
- Party B: signed.
