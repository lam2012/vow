# Language Charter (LOCKED)

Status: LOCKED. Signed by both design parties. Phase 1 is closed.
Phase 1 output.

## 1. Positioning Statement

This language is a from-scratch programming language for authors of
command-line tools and internal utilities who distribute on Windows, Linux,
and macOS. It earns trust through reproducible builds and explicit
dependencies, not through novelty of syntax.

The first user group is internal CLI and tool authors: their feedback loop is
short, their distribution pain (multi-OS packaging) is exactly what the
language guarantees, and their adoption does not require a mature package
registry.

## 2. Design Principles

1. **Self-contained by default.** The standard library and toolchain build
   from the language's own source tree with no network access and no opaque
   binary blobs at release-build time.
2. **Vendoring-first.** External material, when used, is vendored into the
   source tree with provenance recorded, so every dependency is auditable.
   Reuse of knowledge (familiar JSON, HTTP, filesystem API shapes; ported
   algorithms checked against standard vectors with sources cited) is
   encouraged. Hidden, unverifiable dependence is forbidden.
3. **Explicit effects and dependencies.** All effects (I/O, network, files,
   time, randomness, large allocation) and all dependencies are visible,
   version-pinned, and reproducible. No hidden imports, no code execution at
   import time, no behavior that varies by machine or network. Enforcement
   is twofold: the compiler rejects undeclared effects or dependencies at
   compile time, and the conformance suite proves that one source tree
   builds to equivalent, hash-verified results on Windows, Linux, and
   macOS.
4. **Reproducible builds.** A given source tree produces the same result on
   every machine. Reproducibility is checked by machine, not by convention.
5. **Conservative surface, radical guarantees.** Syntax, the type system
   (type inference plus basic algebraic types), and the memory model (safe
   by default, no full borrow-checker at inception) stay close to widespread
   habits. Strictness lives in the guarantees, not in the surface.
6. **Stable per-OS ABI.** Each supported OS gets a stable runtime ABI
   (`.so`, `.dll`, `.dylib`). Cross-OS behavior is verified by a conformance
   suite runnable with a single command on all three systems.
7. **Test-suite-first trust.** A runnable conformance suite precedes prose
   specification. Compatibility promises are issued only against suite
   versions. The package registry comes last, after the core is stable.

## 3. Architecture (Three Layers)

1. **Bootstrap toolchain.** A minimal core written in a conservative C
   subset (auditable, portable, stable ABI) plus cross-build orchestration
   using the Zig build system (currently the most compact Windows / Linux /
   macOS cross-compilation path). The C core is the auditable root; Zig is
   the build layer, not the bootstrap lock-in. Self-hosting follows once the
   minimal core runs; C then remains only a fallback backend. The closed
   scope of the stage-0 core is ratified: lexer, parser, and a minimal
   evaluator/VM sufficient to run its own build script and test runner (the
   test runner counts as part of the core). The evaluator is defined by a
   capability allowlist: read files under declared paths, spawn declared
   processes, hash content, and check lockfiles; no network access. Package
   manager, formatter, and language server are explicitly out.
2. **Runtime ABI.** One stable ABI per OS for dynamic libraries, verified by
   the conformance suite on each OS.
3. **Distribution.** One installer package per OS, all built from the same
   source tree, with vendored dependencies included.

## 4. Extreme Where, Friendly Where

| Area | Stance |
| ---- | ------ |
| Dependency and effect model | Extreme: everything visible, pinned, reproducible |
| Reproducibility enforcement | Extreme: machine-checked |
| Syntax | Friendly: close to widespread habits |
| Type system | Friendly: pragmatic inference, basic algebraic types |
| Memory model | Friendly at inception: safe defaults, no mandatory borrow-checking |
| Migration and interop | Friendly: familiar API shapes, good tooling |
| Package registry | Deferred until the core is stable |

## 5. Phase Gates

- **Phase 1 (closed):** this charter signed off by both parties; resolutions
  recorded in Section 6.
- **Phase 2:** CLI-proven cross-build and distribution, followed
  immediately by dynamic-library ABI proof (`.so` / `.dll` / `.dylib`).
  The ABI gate is mandatory; it is ordered after CLI validation, not
  dropped.
- **Later:** compatibility promise (LTS-style), then package registry.

## 6. Phase 1 Resolutions and Success Metrics

1. Stage-0 core scope: ratified as stated in Section 3, including the
   evaluator capability allowlist.
2. Explicit-effect enforcement: both mechanisms with distinct roles —
   compiler static check (early rejection at the point of code) and
   conformance suite (reproducibility proof across the three OSes).
3. Success metrics: one command runs the suite green on Windows, Linux, and
   macOS; artifacts are reproducibility-checked by hash; three sample CLIs
   install through the per-OS installers in an offline environment.

## 7. Sign-off

- Party A: signed.
- Party B: signed.
