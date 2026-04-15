#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "parser.h"

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
    remove_if_exists("build/test_executor_db/users.schema");
    remove_if_exists("build/test_executor_db/users.data");
    RMDIR("build/test_executor_db");
}

static void prepare_test_db(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");

    schema_file = fopen("build/test_executor_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

static char *read_entire_file(const char *path)
{
    FILE *file;
    long length;
    char *buffer;
    size_t read_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0L) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char *)malloc((size_t)length + 1U);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1U, (size_t)length, file);
    fclose(file);

    if (read_size != (size_t)length) {
        free(buffer);
        return NULL;
    }

    buffer[length] = '\0';
    return buffer;
}

static char *dup_string(const char *src)
{
    size_t len = strlen(src) + 1U;
    char *copy = (char *)malloc(len);

    if (copy == NULL) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    memcpy(copy, src, len);
    return copy;
}

static Statement make_insert_stmt(const char *table_name,
                                  const char **columns, size_t column_count,
                                  const char **values, size_t value_count)
{
    Statement stmt = {0};
    size_t i;

    stmt.type = STMT_INSERT;
    stmt.insert_stmt.table_name = dup_string(table_name);
    stmt.insert_stmt.column_count = column_count;
    stmt.insert_stmt.value_count = value_count;

    if (column_count > 0U) {
        stmt.insert_stmt.columns = (char **)calloc(column_count, sizeof(char *));
        if (stmt.insert_stmt.columns == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }

        for (i = 0U; i < column_count; i++) {
            stmt.insert_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (value_count > 0U) {
        stmt.insert_stmt.values = (LiteralValue *)calloc(value_count, sizeof(LiteralValue));
        if (stmt.insert_stmt.values == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }

        for (i = 0U; i < value_count; i++) {
            stmt.insert_stmt.values[i].type = VALUE_STRING;
            stmt.insert_stmt.values[i].text = dup_string(values[i]);
        }
    }

    return stmt;
}

static Statement make_select_stmt(const char *table_name,
                                  int select_all,
                                  const char **columns, size_t column_count,
                                  const char *where_column,
                                  const char *where_value)
{
    Statement stmt = {0};
    size_t i;

    stmt.type = STMT_SELECT;
    stmt.select_stmt.table_name = dup_string(table_name);
    stmt.select_stmt.select_all = select_all;
    stmt.select_stmt.column_count = column_count;

    if (column_count > 0U) {
        stmt.select_stmt.columns = (char **)calloc(column_count, sizeof(char *));
        if (stmt.select_stmt.columns == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }

        for (i = 0U; i < column_count; i++) {
            stmt.select_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (where_column != NULL) {
        stmt.select_stmt.where_clause.has_condition = 1;
        stmt.select_stmt.where_clause.column_name = dup_string(where_column);
        stmt.select_stmt.where_clause.value.type = VALUE_STRING;
        stmt.select_stmt.where_clause.value.text = dup_string(where_value);
    }

    return stmt;
}

static void seed_users_data(const char *content)
{
    FILE *data_file;

    data_file = fopen("build/test_executor_db/users.data", "w");
    if (data_file == NULL) {
        fprintf(stderr, "Failed to open data file: %s\n", strerror(errno));
        exit(1);
    }

    fputs(content, data_file);
    fclose(data_file);
}

static void test_insert_success(void)
{
    const char *values[] = {"1", "Alice", "20"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};
    char *content;

    prepare_test_db();
    stmt = make_insert_stmt("users", NULL, 0U, values, 3U);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.type == RESULT_INSERT);
    ASSERT_TRUE(result.affected_rows == 1U);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Alice|20\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_select_all_success(void)
{
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    seed_users_data("1|Alice|20\n2|Bob|25\n");
    stmt = make_select_stmt("users", 1, NULL, 0U, NULL, NULL);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.type == RESULT_SELECT);
    ASSERT_TRUE(result.query_result.column_count == 3U);
    ASSERT_TRUE(result.query_result.row_count == 2U);
    ASSERT_STREQ("id", result.query_result.columns[0]);
    ASSERT_STREQ("name", result.query_result.columns[1]);
    ASSERT_STREQ("age", result.query_result.columns[2]);
    ASSERT_STREQ("Bob", result.query_result.rows[1].values[1]);
    ASSERT_STREQ("25", result.query_result.rows[1].values[2]);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_select_partial_success(void)
{
    const char *columns[] = {"name", "id"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    seed_users_data("1|Alice|20\n2|Bob|25\n");
    stmt = make_select_stmt("users", 0, columns, 2U, NULL, NULL);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.query_result.column_count == 2U);
    ASSERT_TRUE(result.query_result.row_count == 2U);
    ASSERT_STREQ("name", result.query_result.columns[0]);
    ASSERT_STREQ("id", result.query_result.columns[1]);
    ASSERT_STREQ("Alice", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("2", result.query_result.rows[1].values[1]);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_select_where_filters_rows(void)
{
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    seed_users_data("1|Alice|20\n2|Bob|25\n");
    stmt = make_select_stmt("users", 1, NULL, 0U, "id", "2");

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_TRUE(result.affected_rows == 1U);
    ASSERT_STREQ("2", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("Bob", result.query_result.rows[0].values[1]);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_unknown_table_error(void)
{
    const char *columns[] = {"id"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");
    stmt = make_select_stmt("users", 0, columns, 1U, NULL, NULL);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_SCHEMA_ERROR);
    ASSERT_TRUE(strstr(errbuf, "SCHEMA ERROR") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_unknown_column_error(void)
{
    const char *columns[] = {"age2"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    stmt = make_select_stmt("users", 0, columns, 1U, NULL, NULL);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "unknown column") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_insert_count_mismatch_error(void)
{
    const char *columns[] = {"id", "name"};
    const char *values[] = {"1"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    stmt = make_insert_stmt("users", columns, 2U, values, 1U);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "column count") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_partial_insert_fills_empty_strings(void)
{
    const char *columns[] = {"name", "id"};
    const char *values[] = {"Bob", "2"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};
    char *content;

    prepare_test_db();
    stmt = make_insert_stmt("users", columns, 2U, values, 2U);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("2|Bob|\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_duplicate_id_error(void)
{
    const char *values[] = {"1", "Bob", "25"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    seed_users_data("1|Alice|20\n");
    stmt = make_insert_stmt("users", NULL, 0U, values, 3U);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "duplicate id") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
}

static void test_missing_id_error(void)
{
    const char *columns[] = {"name"};
    const char *values[] = {"Chris"};
    Statement stmt;
    ExecResult result = {0};
    char errbuf[256] = {0};

    prepare_test_db();
    stmt = make_insert_stmt("users", columns, 1U, values, 1U);

    ASSERT_TRUE(execute_statement("build/test_executor_db", &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "requires a non-empty value") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
}

int main(void)
{
    test_insert_success();
    test_select_all_success();
    test_select_partial_success();
    test_select_where_filters_rows();
    test_unknown_table_error();
    test_unknown_column_error();
    test_insert_count_mismatch_error();
    test_partial_insert_fills_empty_strings();
    test_duplicate_id_error();
    test_missing_id_error();
    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    puts("test_executor: OK");
    return 0;
}
