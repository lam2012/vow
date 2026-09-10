#ifndef VOW_EMIT_H
#define VOW_EMIT_H

#include <stddef.h>

/* ABI slice: `vow build --emit-dylib` and the embedded runner the
   generated stub calls. The dylib reuses the whole interpreter
   (check + eval); no native codegen is claimed anywhere in this
   slice. */

/* Contract: runs the embedded entry (src bytes, display name for
   diagnostics, absolute source root for imports and fs anchoring)
   exactly like `vow run`: checks with main required, calls
   zero-parameter main, returns main's integer masked to a byte.
   Failures print a catalog diagnostic on stderr and return the
   mapped exit class (1/2/3/4/5). Single-threaded, like the
   reference itself; arenas reset per call. */
int emit_run_embedded(const unsigned char *src, size_t len,
                      const char *display, const char *root);

/* Contract: implements `vow build --emit-dylib <file.vow>
   [-o <out>]`. Checks the entry (main required), generates the
   stub, abi.map, and dynamic-list beside the output, then links
   the shared library with the locked flags plus -fPIC -shared.
   Returns the process exit code: 0 with the library written, or
   the failure class (usage E1001, missing entry E1002, missing
   toolchain E1003, check failures propagate, OS/build-step
   failures E5006). On check failure nothing is written. */
int emit_build(const char *vowpath, const char *file,
               const char *out);

#endif
