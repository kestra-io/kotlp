/* test_child.c - tests for exit-code forwarding.
 *
 * Rather than hand-build platform-specific wait status words, we fork real
 * children with known fates and feed the resulting waitpid() status through
 * child_exit_code(), so we validate exactly what the wrapper will return. */
#include "koltp.h"
#include "test.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fork a child that calls _exit(code); return child_exit_code() of its status. */
static int run_exit(int code) {
    pid_t pid = fork();
    if (pid == 0) _exit(code);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return child_exit_code(status);
}

/* Fork a child that stops itself, then is killed by `sig`; return the mapped
 * exit code. */
static int run_signal(int sig) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Block until the signal arrives so the parent can deliver it. */
        for (;;) pause();
    }
    /* Give the child a moment to reach pause(), then signal it. */
    usleep(20000);
    kill(pid, sig);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return child_exit_code(status);
}

static void test_normal_exit_codes(void) {
    CHECK(run_exit(0) == 0);
    CHECK(run_exit(1) == 1);
    CHECK(run_exit(42) == 42);
    /* exit codes are masked to 8 bits by the OS: 256 wraps to 0, 257 to 1 */
    CHECK(run_exit(256) == 0);
    CHECK(run_exit(257) == 1);
}

static void test_signal_exit_codes(void) {
    /* POSIX shell convention: 128 + signal number */
    CHECK(run_signal(SIGTERM) == 128 + SIGTERM);
    CHECK(run_signal(SIGKILL) == 128 + SIGKILL);
    CHECK(run_signal(SIGINT) == 128 + SIGINT);
}

void test_child(void) {
    test_normal_exit_codes();
    test_signal_exit_codes();
}
