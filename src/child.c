/* child.c - spawn the wrapped command with stdout/stderr piped back to us.
 *
 * fork()+exec() is used rather than posix_spawn so we keep precise control of
 * fd wiring; Cosmopolitan polyfills fork() on every supported OS (including
 * Windows), so this stays portable. */
#include "kotlp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int child_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    /* Terminated by a signal: mirror the POSIX shell convention of 128 + N so
     * callers see e.g. 130 for SIGINT, 137 for SIGKILL. */
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

pid_t child_spawn(const kotlp_config *cfg, int *out_fd, int *err_fd) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};

    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        fprintf(stderr, "kotlp: pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Read ends stay in the parent and must not leak into the child. */
    set_cloexec(out_pipe[0]);
    set_cloexec(err_pipe[0]);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "kotlp: fork() failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* child: rewire stdout/stderr to the write ends */
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvp(cfg->argv[0], cfg->argv);
        /* exec only returns on failure */
        fprintf(stderr, "kotlp: cannot execute '%s': %s\n", cfg->argv[0],
                strerror(errno));
        _exit(127);
    }

    /* parent: close write ends, hand back the read ends */
    close(out_pipe[1]);
    close(err_pipe[1]);
    *out_fd = out_pipe[0];
    *err_fd = err_pipe[0];
    return pid;
}
