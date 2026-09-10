# Vow Compatibility Promise

Status: LOCKED. Signed by both design parties. This document states what "compatible"
means for Vow, how versions move, and how breaking changes ship.
It covers the language surface, not project planning; dates and
windows below are the proposal under review.

## 1. Versioning

Releases follow SemVer (`MAJOR.MINOR.PATCH`):

- `MAJOR` — a breaking change as defined in Section 3. Requires a
  joint decision, a migration note, and a full suite re-mint in the
  same change.
- `MINOR` — additive only: new capabilities, new E-codes, new CLI
  flags. Existing programs keep identical observable behavior.
- `PATCH` — bug fixes that restore specified behavior. Any change
  in observable output is a defect in the patch, not a feature.
- `MAJOR` zero (`0.y.z`) is pre-stable: this document makes no promise
  for zero-major lines (best effort only); the promise opens in full at
  `1.0.0`.

## 2. Compatibility Definition

Within one `MAJOR` line, all of the following are stable:

- The E-code catalog: existing codes keep their meaning and exit
  class. New codes may appear only in `MINOR` releases. Recalled
  codes are never reused.
- The CLI surface: subcommands, flags, and exit-code classes of
  `vow check`, `vow run`, `vow test`, and `vow build --emit-dylib`.
- The ABI layout in `docs/abi-spec.md`: symbol names, C-stable
  value representation, and the `abi.map` contract.
- The `.vow` grammar: the accepted language only grows. A program
  that checks clean stays checking clean.
- The diagnostic contract: `file:line:col: EXXXX message` shape
  and the code-plus-line matching rule.

## 3. Breaking-Change Policy

- A breaking change ships only in a `MAJOR` release, announced at
  least one `MINOR` earlier with a migration note.
- The migration note names every affected surface, shows the
  old-versus-new behavior, and gives the mechanical rewrite where
  one exists.
- The conformance lock is re-minted in the same change; a green
  suite on the new `MAJOR` is the acceptance proof, never an
  assertion.

## 4. LTS Window

- The latest two `MAJOR` lines receive critical fixes (wrong-answer
  bugs, security issues, data-loss risks).
- Each line is supported for 12 months after the next `MAJOR`
  release, then frozen. Frozen lines get no changes, including no
  security backports; users pin forward instead.

## 5. Out of Scope

No promise is made here about release dates, platform-tier
expansion, the package registry (charter: last), or performance.
Those arrive through their own gates, never smuggled into a
`MINOR`.

## 6. Sign-off

- Party A: signed.
- Party B: signed.
