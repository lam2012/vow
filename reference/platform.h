#ifndef VOW_PLATFORM_H
#define VOW_PLATFORM_H

/* The single OS seam of the reference implementation. Every
   operating-system call lives behind this interface; the rest of the
   codebase includes no platform headers. POSIX implementation.
   Windows equivalents are documented follow-ups per function, in the
   D033 style: disclosed, never silent. */

#include <stddef.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>

/* Classified OS failures. errno is absorbed inside the seam and
   never leaks out; read plat_err() immediately, it resets on every
   call. */
typedef enum {
  PLAT_OK = 0,
  PLAT_MISSING,
  PLAT_DENIED,
  PLAT_RESOURCE
} PlatErr;

/* Contract: class of the last seam call on this thread. No failure
   modes. */
PlatErr plat_err(void);

/* Spawn with stdout/stderr redirected to already-open files.
   Windows follow-up: CreateProcess with STARTUPINFO handles. */
pid_t plat_spawn_redir(const char *file, char *const argv[], int out_fd,
                       int err_fd);

/* Reap one child with EINTR retry. Normal exit yields the status;
   signaled death yields 128 plus the signal number.
   Windows follow-up: WaitForSingleObject plus GetExitCodeProcess
   (STILL_ACTIVE edge documented there). */
int plat_wait(pid_t pid, int *exit_code);

/* Unique temp file; template needs trailing XXXXXX, rewritten
   in place like mkstemp.
   Windows follow-up: GetTempPathW plus GetTempFileNameW. */
int plat_mktemp(char *tpl);

/* Best-effort remove; failures are ignored by design (temp names
   are unique, leftovers are harmless).
   Windows follow-up: _wunlink. */
void plat_unlink(const char *path);

/* Close a descriptor from plat_mktemp. */
int plat_close(int fd);

/* Opaque-by-convention directory stream (storage owned by caller). */
typedef struct {
  DIR *handle;
} PlatDir;

/* Open for iteration. Missing, denied, and resource failures map to
   their PlatErr classes (ENOENT vs EACCES/EPERM distinguished).
   Windows follow-up: FindFirstFileW; denied-vs-missing maps from
   ERROR_FILE_NOT_FOUND vs ERROR_ACCESS_DENIED. */
int plat_opendir(const char *path, PlatDir *out);

/* Next entry, or null at clean end. A null with plat_err() != OK is
   a mid-iteration I/O failure (loud, never a silent skip).
   Windows follow-up: FindNextFileW. */
struct dirent *plat_readdir(PlatDir *d);

int plat_closedir(PlatDir *d);

/* Existence probe that distinguishes missing from unreadable.
   Windows follow-up: GetFileAttributesW mapping. */
int plat_probe_dir(const char *path);

/* Absolute working directory into out (NUL-terminated, cap bytes).
   Used once by `vow build` to pin the source root into the dylib
   stub, so fs paths anchor identically under `vow run` and dlopen.
   getcwd failure classifies the errno; truncation is a loud
   PLAT_RESOURCE, never silent.
   Windows follow-up: GetCurrentDirectoryW. */
int plat_cwd(char *out, size_t cap);

#endif
