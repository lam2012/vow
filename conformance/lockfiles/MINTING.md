# Fixture Lockfile Minting

One-time authoring record. Every pin below is traceable to the exact
minting command. The reference implementation re-verifies all pins at
runtime; this file exists so the first minting (done before any Vow
implementation exists) stays auditable.

## Tool

- `sha256sum (GNU coreutils) 9.11`, system tool, Linux.
- Manifest command, run from `conformance/`:
  `find . -type f | sort | tr '\n' '\0' | xargs -0 sha256sum`
- Result stored in `lockfiles/fixtures.lock` (49 lines: header plus one
  `<sha256>  <path>` line per file, paths relative to `conformance/`).

## Import pins embedded in fixtures

| Fixture | Target | Pin (sha256 of target at mint time) |
| ------- | ------ | ----------------------------------- |
| `vow/grammar/pass/par-05-import.vow` | `vow/grammar/pass/par-05-mod.vow` | `3eed60fd…034ae60` (full value in `fixtures.lock`) |
| `vow/grammar/fail/par-04b-cycle1.vow` | `vow/grammar/fail/par-04b-cycle2.vow` | `a4a69d82…0d5a5b` (full value in `fixtures.lock`) |

- A true two-way correct-pin cycle is unmintable (each file's hash covers
  its own import line), so `par-04b-cycle2.vow` pins its partner with
  64 zeroes deliberately; the expected exit stays 3 either way, and the
  reason ambiguity is recorded in its `.expected` note.
- `par-04c-bad-hash.vow` pins 64 zeroes deliberately (hash-mismatch case).

## Raw-byte fixtures (written with printf, verified with od)

- `vow/lexer/pass/lex-01-crlf.vow`: CRLF line endings.
  `printf 'test "crlf accepted" {\r\n  assert 1 == 1\r\n}\r\n'`
- `vow/lexer/fail/lex-02c-bad-utf8.vow`: trailing `0xFF 0xFE` bytes.
  `printf 'func main() {\n  return 0\n}\n\xff\xfe\n'`
- `vow/lexer/fail/lex-02d-lone-cr.vow`: lone CR bytes (each `\r` not
  followed by `\n`; the final `\r\n` is an accepted CRLF pair).
  `printf 'func main() {\r  return 0\r}\r\n'`

## Batch-2 note

Batch-2 added 11 fixtures plus 11 mirrored `.expected` files and edited
`driver.vow` (10 new driver assertions; no lines for par-04e and E5007,
see below), `run-01.stdout`, and `run-02.expected` (PENDING lines
removed after R6). Pins for new and edited files were appended/patched
with the same tool and command; the manifest now holds 67 entries.
Deliberately uncovered: E5007 has no fixture (no portable byte producer
exists under current capabilities; ruling pending) and par-04e has no
driver line (it exits 2 while E3002 names exit 3; ruling pending).

## Batch-3 note

Applied both rulings: E3002 recalled and replaced by E2006 (parse error,
non-literal effect scope), E5007 deferred to implementation-level tests.
Batch-3 edited `driver.vow` (new "parse errors exit 2" block),
`par-04e-computed-arg.expected` (E2006 note), and `docs/error-codes.md`
(E2006 row, E5007 row, recalled-code rule, sign-off). Pins for the two
edited conformance files were re-minted with the same tool; the manifest
still holds 67 entries, machine-verified 67/67 with full coverage.

## Line references

`file:line` references in `expected/vow/runner/run-01.stdout` and
`run-02.expected` assume single-line mini-suite files (`:1`). Re-mint
them if any mini-suite file is edited.

## Batch-3 note: driver annotation strip (slice-1 authoring)

- `driver.vow` declared typed parameters (`path string`, `want int`,
  `dir string`), which the locked stage-0 specification does not
  parse: values are dynamically typed (Section 5) and `param_list` is
  bare identifiers (R11 pending). Fixed to bare parameters; no other
  byte changed.
- Re-mint command, run from `conformance/`:
  `sha256sum driver.vow`
  (`sha256sum (GNU coreutils) 9.11`, Linux).
- Previous pin replaced:
  `9581443eaa00801c8346c721070d16d7f5f5cb6de7c9d958d04a2e316e40d24d`
  (the valid batch-2 mint) with:
  `275157d5248c7815cc9a83738f8cdcea5266d57c93edc6c450e98d8ffcadfe29`.

## Batch-4 note: R12/R13 fixtures and driver lines (slice-1 review)

- New files: `vow/effects/fail/eff-12-nonfunc-call.vow`,
  `vow/grammar/fail/par-04h-unknown-name.vow`, plus their two
  `expected/` mirrors.
- `driver.vow` gained two assertions (par-04h expects 3, eff-12
  expects 5); its pin was re-minted with the same tool and command
  as the manifest command above.
- Full re-mint verified by machine: 71/71 pins match with complete
  coverage (every file except the two lockfiles files themselves).

## Batch-5 note: genuine E3003 fixture (checker review)

- New files: `vow/grammar/fail/par-04i-same-dir-bad-hash.vow` (same-dir
  import with a zeroed pin) plus its `expected/` mirror. A cross-dir
  bad pin (par-04c) reports E3006 in practice because root escape is
  decided before any outside file is read; par-04c's note records
  this order.
- `driver.vow` gained the par-04i assertion (expects 3).
- Full re-mint verified by machine: 73/73 pins match with complete
  coverage.

## Batch-6 note: E3009 widening fixtures (checker review)

- New files: `vow/grammar/fail/par-04j-dup-func.vow` (duplicate
  top-level declaration), `vow/grammar/fail/par-04k-reserved-root.vow`
  (capability root as declaration name), plus their two `expected/`
  mirrors (both exit 3).
- `driver.vow` gained the two assertions (both expect 3);
  `par-04c-bad-hash.expected` note corrected to the escape-first
  order. Pins re-minted with the same tool and command.
- Full re-mint verified by machine: 77/77 pins match with complete
  coverage.

## Batch-7 note: slice-3 runtime fixtures (evaluator review)

- New files: `vow/effects/fail/eff-13-missing-file.vow` (E5010),
  `vow/effects/fail/eff-14-dynamic-effect.vow` (E5009),
  `vow/effects/fail/eff-15-type-error.vow` (E5012),
  `vow/effects/fail/eff-17-arity.vow` (E5012),
  `vow/runner/run-04-main-nonint.vow` (E5011), plus their five
  `expected/` mirrors (all exit 5).
- `driver.vow` gained six assertions (eff-13/14/15/17, run-04, plus
  the propagated exit-7 line context unchanged).
- Full re-mint verified by machine: 87/87 pins match with complete
  coverage.

## Batch-8 note: slice-3 review fixtures (evaluator review)

- New files: `vow/grammar/fail/par-04l-bare-alias.vow` (E3009) plus
  its `expected/` mirror; `vow/runner/pass/memo-01a.vow` and
  `vow/runner/pass/memo-01b.vow` (same const name, different values;
  no `expected/` files — pass fixtures assert internally);
  `vow/effects/pass/eff-16-lock-true.vow` (lock.check-true happy path)
  plus `vow/effects/pass/probe.lock` (pins lex-01-tokens.vow).
- `driver.vow` gained the par-04l assertion (expects 3).
- Full re-mint verified by machine: 93/93 pins match with complete
  coverage.

## Batch-10 note: code lines, tripwire, usage tests (reference review)

- Every fail-class `.expected` file gained a trailing `code: EXXXX` line
  (37 files); `run-01.stdout` (ok lines) and `run-03-exit7.expected`
  (exit propagation, no failure) carry none by design.
- New `coverage.sh`: two-way tripwire between `docs/error-codes.md`
  rows and tree references (fixtures, expected `code:` lines, driver,
  `reference/tests.sh`); rows saying implementation-level/evaluator-level
  only are exempt; wired as part 0 of `reference/tests.sh`.
- E1000–E1003 had no reference anywhere, so `reference/tests.sh` gained
  five usage spot checks asserting exact codes (not just the E1 class).
- Pins re-minted by machine for all touched files; `coverage.sh` itself
  is lock-covered.

## Batch-9 note: par-04l import path fix (checker review)

- `vow/grammar/fail/par-04l-bare-alias.vow` imported its target from
  the wrong directory and could only ever fail E3003, never reaching
  the alias line. It now imports the same-directory
  `par-04a-dup-key.vow` with that file's true pin, so the bare alias
  genuinely reports E3009 at 3:9. (`../pass/` cannot work: the root
  is the entry's own directory, so `..` escapes it by rule.)
- Full re-mint verified by machine: 93/93 pins match with complete
  coverage (one pin changed, none added).
