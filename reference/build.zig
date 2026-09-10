// Vow reference build: compiles the C core with the exact audited
// flags and wires the acceptance suite. Replaces the interim
// Makefile one for one: `zig build` builds, `zig build test` builds
// and runs the full suite. Native target by default; pass
// -Dtarget=<triple> to cross-compile (execution stays gated on real
// hardware per the locked charter).
const std = @import("std");

const c_sources = [_][]const u8{
    "main.c",
    "diag.c",
    "lex.c",
    "ast.c",
    "parse.c",
    "check.c",
    "sha256.c",
    "eval.c",
    "run.c",
    "platform.c",
    "emit.c",
};

const c_flags = [_][]const u8{
    "-std=c11",
    "-pedantic",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-O2",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    mod.addCSourceFiles(.{
        .files = &c_sources,
        .flags = &c_flags,
    });
    const exe = b.addExecutable(.{
        .name = "vow",
        .root_module = mod,
    });
    b.installArtifact(exe);

    // `zig build test`: build first, then run tests.sh from this
    // directory with VOW/VOWBIN pointing at the just-built binary
    // and PATH carrying its directory (driver subprocesses spawn
    // `vow`). $PWD expands at run time with the cwd below.
    const suite = b.addSystemCommand(&.{
        "sh",
        "-c",
        "VOW=$PWD/zig-out/bin/vow VOWBIN=$PWD/zig-out/bin/vow PATH=$PWD/zig-out/bin:$PATH sh tests.sh",
    });
    suite.setCwd(b.path(""));
    suite.step.dependOn(b.getInstallStep());
    const test_step = b.step("test", "Build vow and run the acceptance suite");
    test_step.dependOn(&suite.step);
}
