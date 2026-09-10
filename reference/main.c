#include <stddef.h>

#include "check.h"
#include "diag.h"
#include "emit.h"
#include "run.h"

/* Slice-3 driver: `check` lexes/parses/checks, `run` executes main,
   `test` runs collected test blocks, `build --emit-dylib` links
   the entry into a shared library. */

static int arg_is(const char *a, const char *b) {
  size_t i = 0;
  for (;;) {
    if (a[i] != b[i]) {
      return 0;
    }
    if (a[i] == '\0') {
      return 1;
    }
    i += 1;
  }
}

static void usage(void) {
  diag_emit("vow", 1, 1, "E1001",
            "usage: vow check <file.vow> | vow run <file.vow> | "
            "vow test [dir] | vow build --emit-dylib <file.vow> "
            "[-o <out>]");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  if (arg_is(argv[1], "check")) {
    if (argc != 3) {
      usage();
      return 1;
    }
    {
      /* check-only: the run pipeline with main not required. */
      static unsigned char arena_buf[8 * 1024 * 1024];
        Arena arena;
        Checker chk;
        arena_init(&arena, arena_buf, sizeof(arena_buf));
        check_init(&chk, &arena);
        if (!check_file(&chk, argv[2], 0)) {
          diag_emit(chk.err_file, chk.err_line, chk.err_col,
                    chk.err_code, chk.err_msg);
          {
            char c = chk.err_code[1];
            if (c == '2') {
              return 2;
            }
            if (c == '3') {
              return 3;
            }
            if (c == '4') {
              return 4;
            }
            if (c == '5') {
              return 5;
            }
            return 1;
          }
        }
        return 0;
      }
    }
  if (arg_is(argv[1], "run")) {
    if (argc != 3) {
      usage();
      return 1;
    }
    return run_main(argv[0], argv[2]);
  }
  if (arg_is(argv[1], "test")) {
    if (argc != 2 && argc != 3) {
      usage();
      return 1;
    }
    return run_tests(argv[0], (argc == 3) ? argv[2] : "tests/");
  }
  if (arg_is(argv[1], "build")) {
    if (argc != 4 && argc != 6) {
      usage();
      return 1;
    }
    if (!arg_is(argv[2], "--emit-dylib")) {
      usage();
      return 1;
    }
    if (argc == 6) {
      if (!arg_is(argv[4], "-o")) {
        usage();
        return 1;
      }
      return emit_build(argv[0], argv[3], argv[5]);
    }
    return emit_build(argv[0], argv[3], NULL);
  }
  diag_emit("vow", 1, 1, "E1000", "unknown subcommand");
  return 1;
}
