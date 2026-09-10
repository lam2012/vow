# Vow Offline Registry

Status: LOCKED. Signed by both design parties. This document defines package sharing with no
server: vendored sources plus a pinned index. It composes existing
machinery only (relative imports with hash pins, `lock.check`);
it adds no capability, no CLI command, and no conformance surface.

## 1. Vendor Layout

Packages live under the source root at `vendor/<name>/<version>/`:

- `name` is one segment, `[a-z0-9][a-z0-9-]*`.
- `version` is a SemVer core `X.Y.Z` (see `docs/compat-promise.md`).
- Versions are exact only. Ranges do not exist at stage-0; wanting
  a newer release means re-vendoring under its own versioned path.

## 2. Index File

`vendor.lock` sits beside `vendor/` and uses the lockfile line
format (`<sha256>  <path>`, `#` comment lines, paths
root-relative), with one addition: its first line is the marker
`# vow-index: 1`. Every vendored file gets one pin line. The file
is verified today by `lock.check` (comments skipped, entries
root-fenced); no reader change is needed.

## 3. Resolution (No Solver)

Resolving a package is three existing steps, in order:

1. Import the entry by relative path with its pin:
   `import "vendor/<name>/<version>/mod.vow" as m hash "<sha256>"`.
   Mismatch is E3003 at check time (pinned imports: fixture
   par-05).
2. Transitive imports inside `vendor/` resolve relative with their
   own pins, exactly like any other import.
3. `lock.check(["vendor.lock"])` verifies the whole tree offline
   and returns a boolean (lock true path: fixture eff-16).

There is no version solver, no network fetch, and no hidden
state: the tree on disk plus the pins is the entire registry.

## 4. Integrity Chain

Entry pin covers contents, contents carry transitive pins, and the
index covers the tree. One tampered byte anywhere yields E3003 at
check time or a false lock at runtime, never a silent substitution.
Updating a package means re-vendoring plus re-minting pins, a
mechanical step recorded like any lockfile minting.

## 5. Out of Scope

No server or protocol, no version ranges, no new CLI command, no
new capability, no signature scheme (pins are content hashes;
minter trust stays out of band), no conformance fixtures. Each of
these needs a joint decision and its own charter gate.

## Sign-off

- Party A: signed.
- Party B: signed.
