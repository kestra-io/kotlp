/* test_filesink.c - tests for the --log-dir file sink helpers: the rotation
 * file-name builder and the mkdir -p used to create the directory.
 *
 * The sink itself (rotation timing, the tee from otel_emit, the file count on
 * the root span) is covered end-to-end by scripts/smoke_test.sh. */
#include "kotlp.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Without rotation the file is plain log.ndjson; index <= 0 selects it. */
static void test_file_name_no_rotation(void) {
    char name[32];
    kotlp_log_file_name(name, sizeof(name), 0);
    CHECK_STR_EQ(name, "log.ndjson");
    kotlp_log_file_name(name, sizeof(name), -1);
    CHECK_STR_EQ(name, "log.ndjson");
}

/* With --log-flush-interval the index is 1-based: log-1.ndjson, log-2.ndjson... */
static void test_file_name_rotated(void) {
    char name[32];
    kotlp_log_file_name(name, sizeof(name), 1);
    CHECK_STR_EQ(name, "log-1.ndjson");
    kotlp_log_file_name(name, sizeof(name), 2);
    CHECK_STR_EQ(name, "log-2.ndjson");
    kotlp_log_file_name(name, sizeof(name), 42);
    CHECK_STR_EQ(name, "log-42.ndjson");
}

/* A short buffer must truncate and stay NUL-terminated, never overflow. */
static void test_file_name_truncates(void) {
    char small[6] = {0};
    kotlp_log_file_name(small, sizeof(small), 7);
    CHECK(strlen(small) == 5);
    CHECK_STR_EQ(small, "log-7");
    /* degenerate buffers must simply do nothing */
    char guard[4] = {'x', 'x', 'x', 'x'};
    kotlp_log_file_name(guard, 0, 1);
    CHECK(guard[0] == 'x');
    kotlp_log_file_name(NULL, 16, 1);
}

static const char *tmp_base(void) {
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* kotlp_mkdir_p creates missing parents, and succeeds on an existing tree. */
static void test_mkdir_p_nested(void) {
    char root[512], mid[512], leaf[512];
    snprintf(root, sizeof(root), "%s/kotlp-test-mkdir", tmp_base());
    snprintf(mid, sizeof(mid), "%s/a", root);
    snprintf(leaf, sizeof(leaf), "%s/b", mid);
    /* start from a clean slate in case a previous run was interrupted */
    rmdir(leaf);
    rmdir(mid);
    rmdir(root);

    CHECK(kotlp_mkdir_p(leaf) == 0);
    CHECK(is_dir(root));
    CHECK(is_dir(mid));
    CHECK(is_dir(leaf));
    /* idempotent: an existing directory is success, not EEXIST */
    CHECK(kotlp_mkdir_p(leaf) == 0);
    /* a trailing separator must not create an extra empty component */
    char trailing[520];
    snprintf(trailing, sizeof(trailing), "%s/", leaf);
    CHECK(kotlp_mkdir_p(trailing) == 0);

    rmdir(leaf);
    rmdir(mid);
    rmdir(root);
}

static void test_mkdir_p_rejects_empty(void) {
    CHECK(kotlp_mkdir_p(NULL) == -1);
    CHECK(kotlp_mkdir_p("") == -1);
}

/* kotlp_full_write reports a failing write() instead of swallowing it. */
static void test_full_write_reports_failure(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(kotlp_full_write(fds[1], "abc", 3));
    char buf[4] = {0};
    CHECK(read(fds[0], buf, 3) == 3);
    CHECK_STR_EQ(buf, "abc");
    close(fds[0]);
    close(fds[1]);
    /* a closed fd is EBADF: the helper must say so rather than pretend */
    CHECK(!kotlp_full_write(fds[1], "abc", 3));
}

static long count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n') n++;
    fclose(f);
    return n;
}

/* A rotation whose open() fails must not silence the sink: the record goes to
 * the file still open, the index does not advance, and the next write after
 * the directory is writable again completes the rotation. This is the
 * Cloud Run + gcsfuse failure mode where a transient error used to lose every
 * later record, terminal span included. Skipped as root, who ignores modes. */
static void test_rotation_failure_keeps_current_file(void) {
    if (geteuid() == 0) return;
    char dir[512], f1[540], f2[540];
    snprintf(dir, sizeof(dir), "%s/kotlp-test-rotate", tmp_base());
    snprintf(f1, sizeof(f1), "%s/log-1.ndjson", dir);
    snprintf(f2, sizeof(f2), "%s/log-2.ndjson", dir);
    chmod(dir, 0755);
    unlink(f1);
    unlink(f2);
    rmdir(dir);

    kotlp_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_dir = dir;
    cfg.log_flush_interval_s = 1;
    CHECK(filesink_open(&cfg));
    filesink_write("{\"r\":1}", 7);
    CHECK(filesink_file_count() == 1);

    /* make the rotation fail: the directory no longer accepts new files */
    CHECK(chmod(dir, 0555) == 0);
    usleep(1100 * 1000);
    filesink_write("{\"r\":2}", 7); /* rotation due, open(log-2) fails */
    CHECK(filesink_file_count() == 1);
    CHECK(count_lines(f1) == 2);
    CHECK(access(f2, F_OK) != 0);

    /* directory writable again: the very next record completes the rotation */
    CHECK(chmod(dir, 0755) == 0);
    filesink_write("{\"r\":3}", 7);
    CHECK(filesink_file_count() == 2);
    CHECK(count_lines(f1) == 2);
    CHECK(count_lines(f2) == 1);

    filesink_seal();
    filesink_write("{\"r\":4}", 7); /* sealed: no rotation, lands in log-2 */
    filesink_close();
    CHECK(count_lines(f2) == 2);

    unlink(f1);
    unlink(f2);
    rmdir(dir);
}

void test_filesink(void) {
    test_file_name_no_rotation();
    test_file_name_rotated();
    test_file_name_truncates();
    test_mkdir_p_nested();
    test_mkdir_p_rejects_empty();
    test_full_write_reports_failure();
    test_rotation_failure_keeps_current_file();
}
