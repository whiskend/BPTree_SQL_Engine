#include "executor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "schema.h"
#include "storage.h"

static int execute_insert(const char *db_dir, const InsertStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);
static int execute_select(const char *db_dir, const SelectStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size);
static int build_insert_row(const TableSchema *schema, const InsertStatement *stmt,
                            Row *out_row,
                            char *errbuf, size_t errbuf_size);
static int ensure_unique_id_value(const char *db_dir, const TableSchema *schema,
                                  const char *table_name, const Row *row,
                                  char *errbuf, size_t errbuf_size);
static int validate_select_columns(const TableSchema *schema, const SelectStatement *stmt,
                                   char *errbuf, size_t errbuf_size);
static int project_rows_for_select(const TableSchema *schema, const SelectStatement *stmt,
                                   const Row *all_rows, size_t all_row_count,
                                   QueryResult *out_result,
                                   char *errbuf, size_t errbuf_size);
static int row_matches_where_clause(const SelectStatement *stmt, int where_index, const Row *row);
static char *dup_string(const char *src);

static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

static int translate_module_status(int raw_status, int mapped_status,
                                   const char *prefix,
                                   char *errbuf, size_t errbuf_size)
{
    char *copy;

    if (raw_status == STATUS_OK) {
        return STATUS_OK;
    }

    if (errbuf == NULL || errbuf_size == 0) {
        return mapped_status;
    }

    copy = dup_string(errbuf);
    if (copy == NULL) {
        set_error(errbuf, errbuf_size, "%s", prefix);
        return mapped_status;
    }

    if (copy[0] != '\0') {
        set_error(errbuf, errbuf_size, "%s: %s", prefix, copy);
    } else {
        set_error(errbuf, errbuf_size, "%s", prefix);
    }

    free(copy);
    return mapped_status;
}

static char *dup_string(const char *src)
{
    size_t length;
    char *copy;

    if (src == NULL) {
        src = "";
    }

    length = strlen(src);
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, src, length + 1);
    return copy;
}

static void free_row_contents(Row *row)
{
    size_t i;

    if (row == NULL || row->values == NULL) {
        return;
    }

    for (i = 0; i < row->value_count; ++i) {
        free(row->values[i]);
    }

    free(row->values);
    row->values = NULL;
    row->value_count = 0;
}

static void free_query_result_contents(QueryResult *query_result)
{
    size_t row_index;
    size_t column_index;

    if (query_result == NULL) {
        return;
    }

    if (query_result->columns != NULL) {
        for (column_index = 0; column_index < query_result->column_count; ++column_index) {
            free(query_result->columns[column_index]);
        }
        free(query_result->columns);
    }

    if (query_result->rows != NULL) {
        for (row_index = 0; row_index < query_result->row_count; ++row_index) {
            free_row_contents(&query_result->rows[row_index]);
        }
        free(query_result->rows);
    }

    query_result->columns = NULL;
    query_result->column_count = 0;
    query_result->rows = NULL;
    query_result->row_count = 0;
}

int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size)
{
    if (db_dir == NULL || stmt == NULL || out_result == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid execute arguments");
        return STATUS_EXEC_ERROR;
    }

    memset(out_result, 0, sizeof(*out_result));

    switch (stmt->type) {
        case STMT_INSERT:
            return execute_insert(db_dir, &stmt->insert_stmt, out_result, errbuf, errbuf_size);
        case STMT_SELECT:
            return execute_select(db_dir, &stmt->select_stmt, out_result, errbuf, errbuf_size);
        default:
            set_error(errbuf, errbuf_size, "EXEC ERROR: unsupported statement type");
            return STATUS_EXEC_ERROR;
    }
}

void free_exec_result(ExecResult *result)
{
    if (result == NULL) {
        return;
    }

    free_query_result_contents(&result->query_result);
    result->affected_rows = 0;
}

static int execute_insert(const char *db_dir, const InsertStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size)
{
    TableSchema schema = {0};
    Row row = {0};
    int status;

    status = load_table_schema(db_dir, stmt->table_name, &schema, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return translate_module_status(status, STATUS_SCHEMA_ERROR, "SCHEMA ERROR", errbuf, errbuf_size);
    }

    status = build_insert_row(&schema, stmt, &row, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_schema(&schema);
        return status;
    }

    status = ensure_unique_id_value(db_dir, &schema, stmt->table_name, &row, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_row_contents(&row);
        free_table_schema(&schema);
        return status;
    }

    status = ensure_table_data_file(db_dir, stmt->table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_row_contents(&row);
        free_table_schema(&schema);
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    status = append_row_to_table(db_dir, stmt->table_name, &row, errbuf, errbuf_size);
    free_row_contents(&row);
    free_table_schema(&schema);
    if (status != STATUS_OK) {
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    out_result->type = RESULT_INSERT;
    out_result->affected_rows = 1;
    return STATUS_OK;
}

static int execute_select(const char *db_dir, const SelectStatement *stmt,
                          ExecResult *out_result,
                          char *errbuf, size_t errbuf_size)
{
    TableSchema schema = {0};
    Row *all_rows = NULL;
    size_t all_row_count = 0;
    int status;

    status = load_table_schema(db_dir, stmt->table_name, &schema, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return translate_module_status(status, STATUS_SCHEMA_ERROR, "SCHEMA ERROR", errbuf, errbuf_size);
    }

    status = validate_select_columns(&schema, stmt, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_schema(&schema);
        return status;
    }

    status = read_all_rows_from_table(db_dir, stmt->table_name, schema.column_count,
                                      &all_rows, &all_row_count, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_table_schema(&schema);
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    status = project_rows_for_select(&schema, stmt, all_rows, all_row_count,
                                     &out_result->query_result, errbuf, errbuf_size);
    free_rows(all_rows, all_row_count);
    free_table_schema(&schema);
    if (status != STATUS_OK) {
        return status;
    }

    out_result->type = RESULT_SELECT;
    out_result->affected_rows = out_result->query_result.row_count;
    return STATUS_OK;
}

static int build_insert_row(const TableSchema *schema, const InsertStatement *stmt,
                            Row *out_row,
                            char *errbuf, size_t errbuf_size)
{
    size_t i;
    int *seen_columns;

    if (schema == NULL || stmt == NULL || out_row == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid insert arguments");
        return STATUS_EXEC_ERROR;
    }

    if (stmt->column_count == 0) {
        if (stmt->value_count != schema->column_count) {
            set_error(errbuf, errbuf_size,
                      "EXEC ERROR: expected %zu values but got %zu",
                      schema->column_count, stmt->value_count);
            return STATUS_EXEC_ERROR;
        }
    } else if (stmt->column_count != stmt->value_count) {
        set_error(errbuf, errbuf_size,
                  "EXEC ERROR: column count %zu does not match value count %zu",
                  stmt->column_count, stmt->value_count);
        return STATUS_EXEC_ERROR;
    }

    out_row->values = (char **)calloc(schema->column_count, sizeof(char *));
    if (out_row->values == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }
    out_row->value_count = schema->column_count;

    for (i = 0; i < schema->column_count; ++i) {
        out_row->values[i] = dup_string("");
        if (out_row->values[i] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free_row_contents(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    if (stmt->column_count == 0) {
        for (i = 0; i < schema->column_count; ++i) {
            free(out_row->values[i]);
            out_row->values[i] = dup_string(stmt->values[i].text);
            if (out_row->values[i] == NULL) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
                free_row_contents(out_row);
                return STATUS_EXEC_ERROR;
            }
        }
        return STATUS_OK;
    }

    seen_columns = (int *)calloc(schema->column_count, sizeof(int));
    if (seen_columns == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        free_row_contents(out_row);
        return STATUS_EXEC_ERROR;
    }

    for (i = 0; i < stmt->column_count; ++i) {
        int column_index = schema_find_column_index(schema, stmt->columns[i]);
        if (column_index < 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->columns[i]);
            free(seen_columns);
            free_row_contents(out_row);
            return STATUS_EXEC_ERROR;
        }
        if (seen_columns[column_index] != 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: duplicate column '%s'", stmt->columns[i]);
            free(seen_columns);
            free_row_contents(out_row);
            return STATUS_EXEC_ERROR;
        }

        seen_columns[column_index] = 1;
        free(out_row->values[column_index]);
        out_row->values[column_index] = dup_string(stmt->values[i].text);
        if (out_row->values[column_index] == NULL) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            free(seen_columns);
            free_row_contents(out_row);
            return STATUS_EXEC_ERROR;
        }
    }

    free(seen_columns);
    return STATUS_OK;
}

static int ensure_unique_id_value(const char *db_dir, const TableSchema *schema,
                                  const char *table_name, const Row *row,
                                  char *errbuf, size_t errbuf_size)
{
    Row *existing_rows = NULL;
    size_t existing_row_count = 0;
    size_t row_index;
    int id_index;
    int status;

    if (db_dir == NULL || schema == NULL || table_name == NULL || row == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid id validation arguments");
        return STATUS_EXEC_ERROR;
    }

    id_index = schema_find_column_index(schema, "id");
    if (id_index < 0) {
        return STATUS_OK;
    }

    if ((size_t)id_index >= row->value_count || row->values[id_index] == NULL || row->values[id_index][0] == '\0') {
        set_error(errbuf, errbuf_size, "EXEC ERROR: column 'id' requires a non-empty value");
        return STATUS_EXEC_ERROR;
    }

    status = read_all_rows_from_table(db_dir, table_name, schema->column_count,
                                      &existing_rows, &existing_row_count,
                                      errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return translate_module_status(status, STATUS_STORAGE_ERROR, "STORAGE ERROR", errbuf, errbuf_size);
    }

    for (row_index = 0; row_index < existing_row_count; ++row_index) {
        const char *existing_id = "";

        if ((size_t)id_index < existing_rows[row_index].value_count &&
            existing_rows[row_index].values[id_index] != NULL) {
            existing_id = existing_rows[row_index].values[id_index];
        }

        if (strcmp(existing_id, row->values[id_index]) == 0) {
            set_error(errbuf, errbuf_size, "EXEC ERROR: duplicate id '%s'", row->values[id_index]);
            free_rows(existing_rows, existing_row_count);
            return STATUS_EXEC_ERROR;
        }
    }

    free_rows(existing_rows, existing_row_count);
    return STATUS_OK;
}

static int validate_select_columns(const TableSchema *schema, const SelectStatement *stmt,
                                   char *errbuf, size_t errbuf_size)
{
    size_t i;

    if (schema == NULL || stmt == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid select arguments");
        return STATUS_EXEC_ERROR;
    }

    if (!stmt->select_all && stmt->column_count == 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: empty select column list");
        return STATUS_EXEC_ERROR;
    }

    if (!stmt->select_all) {
        for (i = 0; i < stmt->column_count; ++i) {
            if (schema_find_column_index(schema, stmt->columns[i]) < 0) {
                set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->columns[i]);
                return STATUS_EXEC_ERROR;
            }
        }
    }

    if (stmt->where_clause.has_condition &&
        schema_find_column_index(schema, stmt->where_clause.column_name) < 0) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: unknown column '%s'", stmt->where_clause.column_name);
        return STATUS_EXEC_ERROR;
    }

    return STATUS_OK;
}

static int row_matches_where_clause(const SelectStatement *stmt, int where_index, const Row *row)
{
    const char *current_value;

    if (stmt == NULL || row == NULL) {
        return 0;
    }

    if (!stmt->where_clause.has_condition) {
        return 1;
    }

    if (where_index < 0 || (size_t)where_index >= row->value_count) {
        return 0;
    }

    current_value = row->values[where_index] == NULL ? "" : row->values[where_index];
    return strcmp(current_value, stmt->where_clause.value.text) == 0;
}

static int project_rows_for_select(const TableSchema *schema, const SelectStatement *stmt,
                                   const Row *all_rows, size_t all_row_count,
                                   QueryResult *out_result,
                                   char *errbuf, size_t errbuf_size)
{
    size_t i;
    size_t row_index;
    size_t selected_count;
    size_t matched_row_count;
    int *selected_indexes = NULL;
    int where_index = -1;

    if (schema == NULL || stmt == NULL || out_result == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: invalid projection arguments");
        return STATUS_EXEC_ERROR;
    }

    selected_count = stmt->select_all ? schema->column_count : stmt->column_count;
    out_result->columns = (char **)calloc(selected_count, sizeof(char *));
    if (selected_count > 0 && out_result->columns == NULL) {
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    selected_indexes = (int *)calloc(selected_count, sizeof(int));
    if (selected_count > 0 && selected_indexes == NULL) {
        free(out_result->columns);
        out_result->columns = NULL;
        set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
        return STATUS_EXEC_ERROR;
    }

    for (i = 0; i < selected_count; ++i) {
        const char *column_name;

        if (stmt->select_all) {
            selected_indexes[i] = (int)i;
            column_name = schema->columns[i];
        } else {
            selected_indexes[i] = schema_find_column_index(schema, stmt->columns[i]);
            column_name = stmt->columns[i];
        }

        out_result->columns[i] = dup_string(column_name);
        if (out_result->columns[i] == NULL) {
            free(selected_indexes);
            free_query_result_contents(out_result);
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            return STATUS_EXEC_ERROR;
        }
    }
    out_result->column_count = selected_count;

    if (stmt->where_clause.has_condition) {
        where_index = schema_find_column_index(schema, stmt->where_clause.column_name);
    }

    if (all_row_count > 0) {
        out_result->rows = (Row *)calloc(all_row_count, sizeof(Row));
        if (out_result->rows == NULL) {
            free(selected_indexes);
            free_query_result_contents(out_result);
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            return STATUS_EXEC_ERROR;
        }
    }

    matched_row_count = 0;
    for (row_index = 0; row_index < all_row_count; ++row_index) {
        size_t output_row_index;

        if (!row_matches_where_clause(stmt, where_index, &all_rows[row_index])) {
            continue;
        }

        output_row_index = matched_row_count;
        out_result->rows[output_row_index].values = (char **)calloc(selected_count, sizeof(char *));
        if (selected_count > 0 && out_result->rows[output_row_index].values == NULL) {
            free(selected_indexes);
            out_result->row_count = matched_row_count;
            free_query_result_contents(out_result);
            set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
            return STATUS_EXEC_ERROR;
        }

        out_result->rows[output_row_index].value_count = selected_count;
        for (i = 0; i < selected_count; ++i) {
            const char *value = "";

            if (selected_indexes[i] >= 0 &&
                (size_t)selected_indexes[i] < all_rows[row_index].value_count) {
                value = all_rows[row_index].values[selected_indexes[i]];
            }

            out_result->rows[output_row_index].values[i] = dup_string(value);
            if (out_result->rows[output_row_index].values[i] == NULL) {
                free(selected_indexes);
                out_result->row_count = matched_row_count + 1U;
                free_query_result_contents(out_result);
                set_error(errbuf, errbuf_size, "EXEC ERROR: out of memory");
                return STATUS_EXEC_ERROR;
            }
        }

        matched_row_count += 1U;
    }

    out_result->row_count = matched_row_count;
    free(selected_indexes);
    return STATUS_OK;
}
