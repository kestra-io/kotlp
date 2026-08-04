/* test_filesink.c - tests for the --log-dir file sink helpers: the rotation
 * file-name builder and the mkdir -p used to create the directory.
 *
 * The sink itself (rotation timing, the tee from otel_emit, the file count on
 * the root span) is covered end-to-end by scripts/smoke_test.sh. */
#include "koltp.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Without rotation the file is plain log.ndjson; index <= 0 selects it. */
static void test_file_name_no_rotation(void) {
    char name[32];
    koltp_log_file_name(name, sizeof(name), 0);
    CHECK_STR_EQ(name, "log.ndjson");
    koltp_log_file_name(name, sizeof(name), -1);
    CHECK_STR_EQ(name, "log.ndjson");
}

/* With --log-flush-interval the index is 1-based: log-1.ndjson, log-2.ndjson... */
static void test_file_name_rotated(void) {
    char name[32];
    koltp_log_file_name(name, sizeof(name), 1);
    CHECK_STR_EQ(name, "log-1.ndjson");
    koltp_log_file_name(name, sizeof(name), 2);
    CHECK_STR_EQ(name, "log-2.ndjson");
    koltp_log_file_name(name, sizeof(name), 42);
    CHECK_STR_EQ(name, "log-42.ndjson");
}

/* A short buffer must truncate and stay NUL-terminated, never overflow. */
static void test_file_name_truncates(void) {
    char small[6] = {0};
    koltp_log_file_name(small, sizeof(small), 7);
    CHECK(strlen(small) == 5);
    CHECK_STR_EQ(small, "log-7");
    /* degenerate buffers must simply do nothing */
    char guard[4] = {'x', 'x', 'x', 'x'};
    koltp_log_file_name(guard, 0, 1);
    CHECK(guard[0] == 'x');
    koltp_log_file_name(NULL, 16, 1);
}

static const char *tmp_base(void) {
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* koltp_mkdir_p creates missing parents, and succeeds on an existing tree. */
static void test_mkdir_p_nested(void) {
    char root[512], mid[512], leaf[512];
    snprintf(root, sizeof(root), "%s/koltp-test-mkdir", tmp_base());
    snprintf(mid, sizeof(mid), "%s/a", root);
    snprintf(leaf, sizeof(leaf), "%s/b", mid);
    /* start from a clean slate in case a previous run was interrupted */
    rmdir(leaf);
    rmdir(mid);
    rmdir(root);

    CHECK(koltp_mkdir_p(leaf) == 0);
    CHECK(is_dir(root));
    CHECK(is_dir(mid));
    CHECK(is_dir(leaf));
    /* idempotent: an existing directory is success, not EEXIST */
    CHECK(koltp_mkdir_p(leaf) == 0);
    /* a trailing separator must not create an extra empty component */
    char trailing[520];
    snprintf(trailing, sizeof(trailing), "%s/", leaf);
    CHECK(koltp_mkdir_p(trailing) == 0);

    rmdir(leaf);
    rmdir(mid);
    rmdir(root);
}

static void test_mkdir_p_rejects_empty(void) {
    CHECK(koltp_mkdir_p(NULL) == -1);
    CHECK(koltp_mkdir_p("") == -1);
}

void test_filesink(void) {
    test_file_name_no_rotation();
    test_file_name_rotated();
    test_file_name_truncates();
    test_mkdir_p_nested();
    test_mkdir_p_rejects_empty();
}
