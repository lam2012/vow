# Vow Installer Specification

Status: LOCKED. Signed by both design parties. This document defines the minimal
distribution layout for the three sample CLIs and how an offline
install is verified. No network access is required at install or at
run time; every byte is hash-pinned.

## 1. Layout

```
vow-<os>-<arch>/
├── vow              # the reference binary (or vow.exe on Windows)
└── cli/
    ├── hello.vow
    ├── hash-file.vow
    ├── verify-lock.vow
    ├── data.txt
    └── data.lock    # sha256 pin of data.txt (64 hex + two spaces + name)
```

- `<os>` and `<arch>` are the Zig target triple components (for
  example `linux-x86_64`, `macos-aarch64`, `windows-x86_64`).
- The `cli` directory is the source root for the three programs;
  `vow run` and `vow test` anchor relative `fs` paths there.
- `data.txt` is the sample payload (`hello vow\n`); `data.lock`
  pins it (`sha256_hex(data.txt) + "  data.txt"`).

## 2. Installation

1. Unpack the archive.
2. Copy or move the `vow` binary to a directory on `PATH`, or
   invoke it via its unpacked path.
3. No other step is required. The CLIs are source files; they run
   directly from the unpacked tree.

## 3. Offline Verification

1. Verify the binary hash against the published SHA-256 for the
   release (out-of-band, for example a signed release note).
2. Run the three CLIs offline:
   ```
   vow run cli/hello.vow        # exit 0
   vow run cli/hash-file.vow    # exit 0
   vow run cli/verify-lock.vow  # exit 0 (lock true)
   vow test cli/                 # pass: 3 fail: 0
   ```
   All four commands must succeed with no network access. The `hash`
   and `lock` CLIs cover the two non-trivial capabilities; `hello`
   covers the pure path.

## 4. Out of Scope

No daemon, service, environment-variable injection, or system-wide
installer is provided at this stage. The layout is the contract;
packaging into platform-native installers (`.deb`, `.msi`, `.pkg`)
is a follow-up that must preserve the byte-identical `cli` tree.

## 5. Sign-off

- Party A: signed.
- Party B: signed.
