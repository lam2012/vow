#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/* POSIX implementation of the seam. errno never escapes: every path
   sets the classification. */

static PlatErr g_err = PLAT_OK;

PlatErr plat_err(void) {
  return g_err;
}

static void set_err(int e) {
  if (e == ENOENT || e == ENOTDIR) {
    g_err = PLAT_MISSING;
  } else if (e == EACCES || e == EPERM) {
    g_err = PLAT_DENIED;
  } else {
    g_err = PLAT_RESOURCE;
  }
}

pid_t plat_spawn_redir(const char *file, char *const argv[], int out_fd,
                       int err_fd) {
  pid_t pid;
  g_err = PLAT_OK;
  pid = fork();
  if (pid < 0) {
    set_err(errno);
    return -1;
  }
  if (pid == 0) {
    /* Child: redirect, then exec. On exec failure a single NUL
       marker byte goes to the captured stderr before exiting 127,
       so the parent tells "declared but unrunnable" apart from a
       real 127 exit (Vow-level protocol, documented at the call
       site in eval.c). */
    if (dup2(out_fd, 1) < 0 || dup2(err_fd, 2) < 0) {
      _exit(127);
    }
    {
      int fd;
      /* Close everything except the redirected standards. Walking
         fd numbers 3..255 is bounded and portable within POSIX;
         higher descriptors do not exist in this program (only the
         two temp files plus stdio). */
      for (fd = 3; fd < 256; fd++) {
        close(fd);
      }
    }
    execvp(file, argv);
    {
      char zero = '\0';
      ssize_t w = write(2, &zero, 1);
      (void)w;
      _exit(127);
    }
  }
  return pid;
}

int plat_wait(pid_t pid, int *exit_code) {
  int status = 0;
  g_err = PLAT_OK;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      set_err(errno);
      return -1;
    }
  }
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *exit_code = 128 + WTERMSIG(status);
  } else {
    g_err = PLAT_RESOURCE;
    return -1;
  }
  return 0;
}

int plat_mktemp(char *tpl) {
  int fd;
  g_err = PLAT_OK;
  fd = mkstemp(tpl);
  if (fd < 0) {
    set_err(errno);
    return -1;
  }
  return fd;
}

void plat_unlink(const char *path) {
  g_err = PLAT_OK;
  unlink(path);
}

int plat_close(int fd) {
  int rc;
  g_err = PLAT_OK;
  rc = close(fd);
  if (rc < 0) {
    set_err(errno);
    return -1;
  }
  return 0;
}

int plat_opendir(const char *path, PlatDir *out) {
  DIR *d;
  g_err = PLAT_OK;
  d = opendir(path);
  if (d == NULL) {
    set_err(errno);
    return -1;
  }
  out->handle = d;
  return 0;
}

struct dirent *plat_readdir(PlatDir *d) {
  struct dirent *ent;
  g_err = PLAT_OK;
  errno = 0;
  ent = readdir(d->handle);
  if (ent == NULL && errno != 0) {
    set_err(errno);
    return NULL;
  }
  return ent;
}

int plat_closedir(PlatDir *d) {
  int rc;
  g_err = PLAT_OK;
  rc = closedir(d->handle);
  d->handle = NULL;
  if (rc < 0) {
    set_err(errno);
    return -1;
  }
  return 0;
}

int plat_probe_dir(const char *path) {
  DIR *d;
  g_err = PLAT_OK;
  d = opendir(path);
  if (d == NULL) {
    set_err(errno);
    return 0;
  }
  closedir(d);
  return 1;
}

int plat_cwd(char *out, size_t cap) {
  g_err = PLAT_OK;
  if (cap == 0) {
    g_err = PLAT_RESOURCE;
    return -1;
  }
  /* getcwd fails loudly (never a silent truncation): a null return
     classifies the errno, and an exactly-full buffer without room
     for NUL is a resource breach. */
  if (getcwd(out, cap) == NULL) {
    set_err(errno);
    return -1;
  }
  {
    size_t i = 0;
    while (out[i] != '\0') {
      i += 1;
    }
    if (i + 1 >= cap) {
      g_err = PLAT_RESOURCE;
      return -1;
    }
  }
  return 0;
}
