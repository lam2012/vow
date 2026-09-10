#ifndef VOW_DIAG_H
#define VOW_DIAG_H

/* Diagnostics carry catalog codes from docs/error-codes.md and print as
   file:line:col: EXXXX message on stderr. All messages are static
   literals; there is no formatted output in slice 1. */

void diag_emit(const char *file, int line, int col,
               const char *code, const char *msg);

#endif
