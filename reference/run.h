#ifndef VOW_RUN_H
#define VOW_RUN_H

/* Slice-3 test runner: collects test blocks under a target directory
   and executes them. Collection is recursive; files and directories
   whose names start with '_' are skipped, as are directories named
   exactly 'fail' (negative cases run exclusively through driver
   assertions, R17). */

/* Contract: runs every test under dir (source root included for
   path anchoring). Prints ok/not-ok lines plus a summary, all with
   root-relative '/'-separated paths. Returns the process exit code:
   0 when every collected test passes, 4 otherwise. */
int run_tests(const char *vowpath, const char *dir);

/* Contract: runs the main function of one file with main presence
   enforced (E3007). Prints nothing on success. Returns the process
   exit code: main's integer return, or the failure class. */
int run_main(const char *vowpath, const char *file);

#endif
