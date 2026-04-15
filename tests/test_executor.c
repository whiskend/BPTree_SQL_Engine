#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "parser.h"
#include "runtime.h"

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

static char *dup_string(const char *text)
{
    size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);

    if (copy == NULL) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    memcpy(copy, text, length);
    return copy;
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
    remove_if_exists("build/test_executor_db/products.schema");
    remove_if_exists("build/test_executor_db/products.data");
    RMDIR("build/test_executor_db");
}

static void prepare_users_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");

    schema_file = fopen("build/test_executor_db/users.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open users schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("id\nname\nage\n", schema_file);
    fclose(schema_file);
}

static void prepare_products_schema(void)
{
    FILE *schema_file;

    cleanup_test_db();
    ensure_directory("build");
    ensure_directory("build/test_executor_db");

    schema_file = fopen("build/test_executor_db/products.schema", "w");
    if (schema_file == NULL) {
        fprintf(stderr, "Failed to open products schema file: %s\n", strerror(errno));
        exit(1);
    }

    fputs("name\nprice\n", schema_file);
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
        for (i = 0U; i < column_count; ++i) {
            stmt.insert_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (value_count > 0U) {
        stmt.insert_stmt.values = (LiteralValue *)calloc(value_count, sizeof(LiteralValue));
        if (stmt.insert_stmt.values == NULL) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        for (i = 0U; i < value_count; ++i) {
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
                                  const char *where_value,
                                  ValueType where_type)
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

        for (i = 0U; i < column_count; ++i) {
            stmt.select_stmt.columns[i] = dup_string(columns[i]);
        }
    }

    if (where_column != NULL) {
        stmt.select_stmt.where_clause.has_condition = 1;
        stmt.select_stmt.where_clause.column_name = dup_string(where_column);
        stmt.select_stmt.where_clause.value.type = where_type;
        stmt.select_stmt.where_clause.value.text = dup_string(where_value);
    }

    return stmt;
}

static ExecutionContext create_context(void)
{
    ExecutionContext ctx = {0};
    char errbuf[256] = {0};

    if (init_execution_context("build/test_executor_db", &ctx, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "Failed to init execution context: %s\n", errbuf);
        exit(1);
    }

    return ctx;
}

static void test_auto_id_insert_success(void)
{
    const char *values[] = {"Alice", "20"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};
    char *content;

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", NULL, 0U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.has_generated_id == 1);
    ASSERT_TRUE(result.generated_id == 1U);
    ASSERT_TRUE(result.affected_rows == 1U);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Alice|20\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void test_column_list_auto_id_insert_success(void)
{
    const char *columns[] = {"age", "name"};
    const char *values[] = {"21", "Bob"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};
    char *content;

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", columns, 2U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.generated_id == 1U);

    content = read_entire_file("build/test_executor_db/users.data");
    ASSERT_TRUE(content != NULL);
    ASSERT_STREQ("1|Bob|21\n", content);

    free(content);
    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void test_explicit_id_insert_fails(void)
{
    const char *columns[] = {"id", "name"};
    const char *values[] = {"7", "Alice"};
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    stmt = make_insert_stmt("users", columns, 2U, values, 2U);

    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_EXEC_ERROR);
    ASSERT_TRUE(strstr(errbuf, "explicit 'id'") != NULL);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void seed_users_with_auto_ids(ExecutionContext *ctx)
{
    const char *values1[] = {"Alice", "20"};
    const char *values2[] = {"Bob", "21"};
    Statement stmt1 = make_insert_stmt("users", NULL, 0U, values1, 2U);
    Statement stmt2 = make_insert_stmt("users", NULL, 0U, values2, 2U);
    ExecResult result = {0};
    char errbuf[256] = {0};

    if (execute_statement(ctx, &stmt1, &result, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "seed insert 1 failed: %s\n", errbuf);
        exit(1);
    }
    free_exec_result(&result);

    if (execute_statement(ctx, &stmt2, &result, errbuf, sizeof(errbuf)) != STATUS_OK) {
        fprintf(stderr, "seed insert 2 failed: %s\n", errbuf);
        exit(1);
    }
    free_exec_result(&result);

    free_statement(&stmt1);
    free_statement(&stmt2);
}

static void test_select_where_id_uses_index(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "id", "2", VALUE_NUMBER);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 1);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("2", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("Bob", result.query_result.rows[0].values[1]);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void test_select_where_non_id_uses_full_scan(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "name", "Bob", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("2", result.query_result.rows[0].values[0]);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void test_non_canonical_id_literal_does_not_use_index(void)
{
    Statement stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_users_schema();
    ctx = create_context();
    seed_users_with_auto_ids(&ctx);

    stmt = make_select_stmt("users", 1, NULL, 0U, "id", "001", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 0U);

    free_exec_result(&result);
    free_statement(&stmt);
    free_execution_context(&ctx);
}

static void test_non_indexed_table_keeps_existing_behavior(void)
{
    const char *values[] = {"apple", "1000"};
    Statement insert_stmt;
    Statement select_stmt;
    ExecResult result = {0};
    ExecutionContext ctx;
    char errbuf[256] = {0};

    prepare_products_schema();
    ctx = create_context();

    insert_stmt = make_insert_stmt("products", NULL, 0U, values, 2U);
    ASSERT_TRUE(execute_statement(&ctx, &insert_stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.has_generated_id == 0);
    free_exec_result(&result);
    free_statement(&insert_stmt);

    select_stmt = make_select_stmt("products", 1, NULL, 0U, "name", "apple", VALUE_STRING);
    ASSERT_TRUE(execute_statement(&ctx, &select_stmt, &result, errbuf, sizeof(errbuf)) == STATUS_OK);
    ASSERT_TRUE(result.used_index == 0);
    ASSERT_TRUE(result.query_result.row_count == 1U);
    ASSERT_STREQ("apple", result.query_result.rows[0].values[0]);
    ASSERT_STREQ("1000", result.query_result.rows[0].values[1]);

    free_exec_result(&result);
    free_statement(&select_stmt);
    free_execution_context(&ctx);
}

int main(void)
{
    test_auto_id_insert_success();
    test_column_list_auto_id_insert_success();
    test_explicit_id_insert_fails();
    test_select_where_id_uses_index();
    test_select_where_non_id_uses_full_scan();
    test_non_canonical_id_literal_does_not_use_index();
    test_non_indexed_table_keeps_existing_behavior();
    cleanup_test_db();

    if (tests_failed != 0) {
        return 1;
    }

    puts("test_executor: OK");
    return 0;
}
