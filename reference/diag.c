#include "diag.h"

#include <stdio.h>

/* Contract: prints one diagnostic line to stderr. Inputs are a file
   path, 1-based line and column, a catalog code, and a static message.
   Failure modes: none (output errors are ignored; the exit code still
   reports the failure class). */
void diag_emit(const char *file, int line, int col,
               const char *code, const char *msg) {
  fprintf(stderr, "%s:%d:%d: %s %s\n", file, line, col, code, msg);
}
