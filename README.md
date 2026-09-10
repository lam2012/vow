# Vow

Vow is a small programming language for CLI and tool authors. Every
effect (file, process, hash, lock) and every dependency is explicit,
pinned by hash, and reproducible: the same source tree produces the
same result on every machine. There is no network, no clock, and no
hidden state in the language.

Status: `0.1.0`, pre-stable (see `docs/compat-promise.md`). The
current release is recorded in `docs/release-0.1.0.md`: reference
binary SHA-256 `0cf25529…43a4a521` (linux-x86_64), acceptance suite
part12 87/87, gate 17/17, partABI 15/15, partE5007 6/6, coverage
clean — identical under gcc with AddressSanitizer and UBSan.

## Build and verify

Requires a Zig toolchain (0.16.0 verified) and a C11 compiler. The
ABI suite part additionally needs `cc`, `python3`, `nm`, and `awk`.

```
zig build        # builds reference/vow
zig build test   # builds, then runs the full acceptance suite
```

The core commands:

```
vow check <file.vow>   # lex, parse, check
vow run <file.vow>     # check, then run main
vow test [dir]         # run collected test blocks
vow build --emit-dylib <file.vow> [-o <out>]  # link a shared library
```

Try the three sample CLIs (offline, no network involved):

```
vow run examples/cli/hello.vow
vow run examples/cli/hash-file.vow
vow run examples/cli/verify-lock.vow
vow test examples/cli/
```

## Layout

- `reference/` — the reference implementation in conservative C11
  plus `build.zig` and the acceptance script `tests.sh`.
- `conformance/` — the conformance suite: fixtures, expected
  outputs, hash lockfiles, and the coverage tripwire.
- `examples/cli/` — three sample command-line programs and their
  data pins.
- `docs/` — all design documents (all LOCKED; naming recorded as
  decided): charter, stage-0 specification, error-code catalog, C
  subset, ABI specification, installer specification, compatibility
  promise, offline registry, naming decision, and release notes.

## Scope and limits, stated plainly

- Provenance of every number above: Linux x86-64. The C code is
  POSIX with Windows follow-ups documented in the platform seam;
  other operating systems are not proven yet.
- The language has no network, clock, floating point, or string
  synthesis at stage-0; values are integers, UTF-8 strings,
  booleans, lists, and maps.
- There is one reference implementation and one suite. A second
  C11 compiler (clang 22.1.8) builds the tree clean and runs the
  suite green; its address-sanitizer build diverges on exactly one
  deep-recursion vector (native stack vs. the 1024-level guard),
  recorded in `docs/c-subset.md`.
