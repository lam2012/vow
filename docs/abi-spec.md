# Vow ABI Specification — Minimal

Status: LOCKED. Signed by both design parties. This document defines the minimal stable
ABI for Vow dynamic libraries. It covers the C-stable layout that
every OS build must preserve, the mangling rule, and how the
artifact is produced and smoke-tested offline. No new capability is
added; the existing six capabilities remain the only effects.

## 1. Scope

- One stable ABI per OS: `.so` on Linux, `.dylib` on macOS, `.dll`
  on Windows. The three artifacts are built from the same source
  tree; their bit-identical source pin is the release identifier.
- The ABI surface is C-stable: a caller compiled against one
  toolchain version can `dlopen` a library built with a later
  toolchain version that preserves this spec. Breaking the layout
  requires a major version bump.

## 2. Layout

- Exported symbols are `extern "C"` with no C++ decoration.
- Mangling is limited to a single prefix `vow_` followed by the
  dotted Vow name with dots replaced by underscores. Examples:
  `vow_hello`, `vow_hash_file`, `vow_verify_lock`. No overload,
  no generic specialization, no name compression.
- Calling convention is the platform C call (`cdecl` on x86-64
  System V, `stdcall` equivalent on Windows via the normal C
  declaration — the compiler, not the spec, chooses the register
  mapping).
- Value representation across the boundary is C-stable: integers as
  `int64_t`/`uint64_t`, strings as (`const unsigned char *` plus
  `size_t` length) with no NUL termination assumption, booleans as
  `int` 0/1. No Vow `Val` header crosses the boundary.
- The library exports exactly the symbols listed in the accompanying
  `abi.map` (one name per line, sorted, generated beside the output).
  Any other symbol is not part of the ABI.

## 3. Build

- `vow build --emit-dylib` produces the library for the host OS.
  The command is offline, deterministic, and hash-pinned like every
  other `vow build` artifact: the same source tree on the same OS
  yields a bit-identical file.
- Flags are the locked C-subset flags (`-std=c11 -pedantic -Wall
  -Wextra -Werror -O2`) plus the platform link flags for a shared
  library (`-shared` on POSIX, `/DLL` on Windows via the Zig
  driver). No LTO, no embedded timestamps.

## 4. Smoke Test

Offline, on the host OS:

```
vow build --emit-dylib examples/cli/hello.vow
python3 -c "import ctypes; lib=ctypes.CDLL('./hello.so'); assert lib.vow_hello() == 0"
```

- `dlopen` (or `LoadLibrary` on Windows) must succeed.
- Calling the exported symbol must return the value `main` would
  return (0 for the three sample CLIs).
- The same `.c` source built as an executable and as a dylib must
  produce the identical return value.

## 5. Out of Scope

No new capability, no semantic change, no reference-count or GC
exposure across the boundary, no C++ exception, no async, no
platform-specific quirk beyond the normal C ABI. Adding any of
these requires a joint decision and a charter gate.

## 6. Acceptance

- `vow build --emit-dylib` on the three sample CLIs produces
  `.so`/`.dll`/`.dylib` per OS, bit-identical for the same OS on
  repeated builds.
- The `dlopen` smoke above passes on the host OS offline.
- No conformance lock moves: the ABI is additive, the existing
  94/94 pins stay green.

## Sign-off

- Party A: signed.
- Party B: signed.
