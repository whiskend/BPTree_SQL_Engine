#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "runtime.h"
#include "schema.h"
#include "storage.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(path) mkdir((path), 0777)
#define RMDIR(path) rmdir(path)
#endif

static int tests_failed = 0;

static void fail_test(const char *message, const char *file, int line)
{
    fprintf(stderr, "TEST FAILED at %s:%d: %s\n", file, line, message);
    tests_failed = 1;
}

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fail_test(#expr, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

#define ASSERT_STREQ(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        char _message[512]; \
        snprintf(_message, sizeof(_message), "expected '%s' but got '%s'", (expected), (actual)); \
        fail_test(_message, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

typedef struct {
    size_t seen_rows;
    long target_offset;
} OffsetCapture;

static void remove_if_exists(const char *path)
{
    remove(path);
}

static void ensure_directory(const char *path)
{
    if (MKDIR(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

static void cleanup_test_db(void)
{
    remove_if_exists("build/test_runtime_db/users.schema");
    remove_if_exists("build/test_runtime_db/users.data");
    RMDIR("build/test_runtime_db");
}

static void write_users_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_runtime_db");

    schema_file = fopen("build/test_runtime_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open runtime schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

static void write_users_data(const char *content)
{
    FILE *data_file = fopen("build/test_runtime_db/users.data", "w");

    if (data_file == NULL) {
        fprintf(stderr, "Failed to open runtime data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs(content, data_file);
    fclose(data_file);
}

static int capture_second_row_offset(const Row *row,
                                     long row_offset,
                                     void *user_data,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    OffsetCapture *capture = (OffsetCapture *)user_data;

    (void)row;
    (void)errbuf;
    (void)errbuf_size;

    capture->seen_rows += 1U;
    if (capture->seen_rows == 2U) {
        capture->target_offset = row_offset;
        return 1;
    }

    return 0;
}

static void test_build_index_and_next_id_success(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};
    long offset = 0L;
    int found = 0;
    Row row = {0};

    write_users_schema();
    write_users_data("1|Alice|20\n2|Bob|25\n5|Chris|30\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(table->has_id_column == 1);
    ASSERT_TRUE(table->id_index_ready == 1);
    ASSERT_TRUE(table->next_id == 6U);
    ASSERT_TRUE(bptree_search(&table->id_index, 2U, &offset, &found, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(found == 1);
    ASSERT_TRUE(read_row_at_offset("build/test_runtime_db", "users", offset, 3U, &row, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_STREQ("2", row.values[0]);
    ASSERT_STREQ("Bob", row.values[1]);
    ASSERT_STREQ("25", row.values[2]);

    free_row(&row);
    free_execution_context(&ctx);
}

static void test_duplicate_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("1|Alice|20\n1|Bob|25\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

static void test_empty_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("|Alice|20\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

static void test_malformed_id_build_fails(void)
{
    ExecutionContext ctx = {0};
    TableRuntime *table = NULL;
    char errbuf[256] = {0};

    write_users_schema();
    write_users_data("001|Alice|20\n");

    ASSERT_TRUE(init_execution_context("build/test_runtime_db", &ctx, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(get_or_load_table_runtime(&ctx, "users", &table, errbuf, sizeof(errbuf)) == STATUS_INDEX_ERROR);
    free_execution_context(&ctx);
}

static void test_read_row_at_offset_round_trip(void)
{
    Row read_back = {0};
    char *values1[] = {"1", "Alice", "20"};
    char *values2[] = {"2", "Bob|Builder", "line1\nline2"};
    Row row1 = {values1, 3U};
    Row row2 = {values2, 3U};
    OffsetCapture capture = {0};
    char errbuf[256] = {0};

    write_users_schema();
    ASSERT_TRUE(append_row_to_table("build/test_runtime_db", "users", &row1, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(append_row_to_table("build/test_runtime_db", "users", &row2, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(scan_table_rows_with_offsets("build/test_runtime_db", "users", 3U,
                                             capture_second_row_offset, &capture,
                                             errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(capture.target_offset >= 0L);
    ASSERT_TRUE(read_row_at_offset("build/test_runtime_db", "users", capture.target_offset, 3U,
                                   &read_back, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_STREQ("2", read_back.values[0]);
    ASSERT_STREQ("Bob|Builder", read_back.values[1]);
    ASSERT_STREQ("line1\nline2", read_back.values[2]);

    free_row(&read_back);
}

int main(void)
{
    test_build_index_and_next_id_success();
    test_duplicate_id_build_fails();
    test_empty_id_build_fails();
    test_malformed_id_build_fails();
    test_read_row_at_offset_round_trip();
    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    puts("test_runtime_index: OK");
    return 0;
}
