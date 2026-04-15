#include "storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"

typedef struct {
    Row **rows;
    size_t *row_count;
} ReadAllRowsState;

static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

static char *dup_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        value = "";
    }

    length = strlen(value);
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length + 1U);
    return copy;
}

static char *build_table_path(const char *db_dir, const char *table_name, const char *suffix)
{
    size_t db_len;
    size_t table_len;
    size_t suffix_len;
    int needs_separator;
    size_t total_len;
    char *path;

    db_len = strlen(db_dir);
    table_len = strlen(table_name);
    suffix_len = strlen(suffix);
    needs_separator = (db_len > 0U && db_dir[db_len - 1U] != '/' && db_dir[db_len - 1U] != '\\') ? 1 : 0;
    total_len = db_len + (size_t)needs_separator + table_len + suffix_len + 1U;

    path = (char *)malloc(total_len);
    if (path == NULL) {
        return NULL;
    }

    snprintf(path, total_len, "%s%s%s%s", db_dir, needs_separator ? "/" : "", table_name, suffix);
    return path;
}

static int read_line(FILE *file, char **out_line)
{
    size_t capacity;
    size_t length;
    char *buffer;
    int ch;

    if (out_line == NULL) {
        return -1;
    }

    *out_line = NULL;
    capacity = 128U;
    length = 0U;
    buffer = (char *)malloc(capacity);
    if (buffer == NULL) {
        return -1;
    }

    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            break;
        }

        if (length + 1U >= capacity) {
            char *grown;

            capacity *= 2U;
            grown = (char *)realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return -1;
            }
            buffer = grown;
        }

        buffer[length++] = (char)ch;
    }

    if (ferror(file) != 0) {
        free(buffer);
        return -1;
    }

    if (ch == EOF && length == 0U) {
        free(buffer);
        return 0;
    }

    if (length > 0U && buffer[length - 1U] == '\r') {
        length--;
    }

    buffer[length] = '\0';
    *out_line = buffer;
    return 1;
}

static int append_char(char **buffer, size_t *length, size_t *capacity, char ch)
{
    if (*buffer == NULL) {
        *capacity = 32U;
        *buffer = (char *)malloc(*capacity);
        if (*buffer == NULL) {
            return 1;
        }
        *length = 0U;
    }

    if (*length + 1U >= *capacity) {
        char *grown;

        *capacity *= 2U;
        grown = (char *)realloc(*buffer, *capacity);
        if (grown == NULL) {
            return 1;
        }
        *buffer = grown;
    }

    (*buffer)[(*length)++] = ch;
    return 0;
}

static int push_field(char ***fields, size_t *field_count, char *field_value)
{
    char **grown;

    grown = (char **)realloc(*fields, (*field_count + 1U) * sizeof(char *));
    if (grown == NULL) {
        return 1;
    }

    grown[*field_count] = field_value;
    *fields = grown;
    *field_count += 1U;
    return 0;
}

static void free_field_array(char **fields, size_t field_count)
{
    size_t i;

    if (fields == NULL) {
        return;
    }

    for (i = 0U; i < field_count; ++i) {
        free(fields[i]);
    }
    free(fields);
}

static int normalize_field_count(char ***fields, size_t *field_count, size_t expected_count)
{
    size_t current_count;
    size_t i;

    if (fields == NULL || field_count == NULL) {
        return 1;
    }

    current_count = *field_count;
    if (current_count == expected_count) {
        return 0;
    }

    if (current_count > expected_count) {
        for (i = expected_count; i < current_count; ++i) {
            free((*fields)[i]);
        }

        if (expected_count == 0U) {
            free(*fields);
            *fields = NULL;
        }

        *field_count = expected_count;
        return 0;
    }

    if (expected_count > 0U) {
        char **grown = (char **)realloc(*fields, expected_count * sizeof(char *));
        if (grown == NULL) {
            return 1;
        }
        *fields = grown;
    }

    for (i = current_count; i < expected_count; ++i) {
        (*fields)[i] = dup_string("");
        if ((*fields)[i] == NULL) {
            free_field_array(*fields, i);
            *fields = NULL;
            *field_count = 0U;
            return 1;
        }
    }

    *field_count = expected_count;
    return 0;
}

static char *escape_field(const char *value)
{
    const char *src;
    size_t out_len;
    char *escaped;
    char *dst;

    src = value == NULL ? "" : value;
    out_len = 0U;
    while (*src != '\0') {
        if (*src == '\\' || *src == '|' || *src == '\n') {
            out_len += 2U;
        } else {
            out_len += 1U;
        }
        src++;
    }

    escaped = (char *)malloc(out_len + 1U);
    if (escaped == NULL) {
        return NULL;
    }

    src = value == NULL ? "" : value;
    dst = escaped;
    while (*src != '\0') {
        if (*src == '\\') {
            *dst++ = '\\';
            *dst++ = '\\';
        } else if (*src == '|') {
            *dst++ = '\\';
            *dst++ = '|';
        } else if (*src == '\n') {
            *dst++ = '\\';
            *dst++ = 'n';
        } else {
            *dst++ = *src;
        }
        src++;
    }

    *dst = '\0';
    return escaped;
}

static char *unescape_field(const char *value)
{
    size_t len;
    char *unescaped;
    size_t src_index;
    size_t dst_index;

    if (value == NULL) {
        return NULL;
    }

    len = strlen(value);
    unescaped = (char *)malloc(len + 1U);
    if (unescaped == NULL) {
        return NULL;
    }

    dst_index = 0U;
    for (src_index = 0U; src_index < len; ++src_index) {
        if (value[src_index] != '\\') {
            unescaped[dst_index++] = value[src_index];
            continue;
        }

        if (src_index + 1U >= len) {
            free(unescaped);
            return NULL;
        }

        src_index++;
        if (value[src_index] == '\\') {
            unescaped[dst_index++] = '\\';
        } else if (value[src_index] == '|') {
            unescaped[dst_index++] = '|';
        } else if (value[src_index] == 'n') {
            unescaped[dst_index++] = '\n';
        } else {
            free(unescaped);
            return NULL;
        }
    }

    unescaped[dst_index] = '\0';
    return unescaped;
}

static int split_escaped_row(const char *line, char ***out_fields, size_t *out_count)
{
    char **fields = NULL;
    size_t field_count = 0U;
    char *current_raw = NULL;
    size_t current_len = 0U;
    size_t current_capacity = 0U;
    int escape_pending = 0;
    const char *cursor;

    if (line == NULL || out_fields == NULL || out_count == NULL) {
        return 1;
    }

    *out_fields = NULL;
    *out_count = 0U;

    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (escape_pending != 0) {
            if (append_char(&current_raw, &current_len, &current_capacity, *cursor) != 0) {
                free_field_array(fields, field_count);
                free(current_raw);
                return 1;
            }
            escape_pending = 0;
            continue;
        }

        if (*cursor == '\\') {
            if (append_char(&current_raw, &current_len, &current_capacity, *cursor) != 0) {
                free_field_array(fields, field_count);
                free(current_raw);
                return 1;
            }
            escape_pending = 1;
            continue;
        }

        if (*cursor == '|') {
            char *field_value;

            if (append_char(&current_raw, &current_len, &current_capacity, '\0') != 0) {
                free_field_array(fields, field_count);
                free(current_raw);
                return 1;
            }

            field_value = unescape_field(current_raw);
            if (field_value == NULL || push_field(&fields, &field_count, field_value) != 0) {
                free(field_value);
                free_field_array(fields, field_count);
                free(current_raw);
                return 1;
            }

            current_len = 0U;
            continue;
        }

        if (append_char(&current_raw, &current_len, &current_capacity, *cursor) != 0) {
            free_field_array(fields, field_count);
            free(current_raw);
            return 1;
        }
    }

    if (escape_pending != 0) {
        free_field_array(fields, field_count);
        free(current_raw);
        return 1;
    }

    if (append_char(&current_raw, &current_len, &current_capacity, '\0') != 0) {
        free_field_array(fields, field_count);
        free(current_raw);
        return 1;
    }

    {
        char *field_value = unescape_field(current_raw);

        if (field_value == NULL || push_field(&fields, &field_count, field_value) != 0) {
            free(field_value);
            free_field_array(fields, field_count);
            free(current_raw);
            return 1;
        }
    }

    free(current_raw);
    *out_fields = fields;
    *out_count = field_count;
    return 0;
}

static int parse_line_into_row(const char *line,
                               size_t expected_column_count,
                               Row *out_row)
{
    char **fields = NULL;
    size_t field_count = 0U;

    if (line == NULL || out_row == NULL) {
        return 1;
    }

    memset(out_row, 0, sizeof(*out_row));
    if (split_escaped_row(line, &fields, &field_count) != 0) {
        return 1;
    }

    if (normalize_field_count(&fields, &field_count, expected_column_count) != 0) {
        free_field_array(fields, field_count);
        return 1;
    }

    out_row->values = fields;
    out_row->value_count = field_count;
    return 0;
}

static int copy_row(const Row *src, Row *dst)
{
    size_t i;

    if (src == NULL || dst == NULL) {
        return 1;
    }

    memset(dst, 0, sizeof(*dst));
    if (src->value_count == 0U) {
        return 0;
    }

    dst->values = (char **)calloc(src->value_count, sizeof(char *));
    if (dst->values == NULL) {
        return 1;
    }
    dst->value_count = src->value_count;

    for (i = 0U; i < src->value_count; ++i) {
        dst->values[i] = dup_string(src->values[i]);
        if (dst->values[i] == NULL) {
            free_row(dst);
            return 1;
        }
    }

    return 0;
}

static int read_all_rows_callback(const Row *row,
                                  long row_offset,
                                  void *user_data,
                                  char *errbuf,
                                  size_t errbuf_size)
{
    ReadAllRowsState *state = (ReadAllRowsState *)user_data;
    Row *grown_rows;

    (void)row_offset;

    if (row == NULL || state == NULL || state->rows == NULL || state->row_count == NULL) {
        set_error(errbuf, errbuf_size, "invalid read-all callback arguments");
        return -1;
    }

    grown_rows = (Row *)realloc(*state->rows, (*state->row_count + 1U) * sizeof(Row));
    if (grown_rows == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate row array");
        return -1;
    }

    *state->rows = grown_rows;
    if (copy_row(row, &grown_rows[*state->row_count]) != 0) {
        set_error(errbuf, errbuf_size, "failed to copy table row");
        return -1;
    }

    *state->row_count += 1U;
    return 0;
}

int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size)
{
    char *data_path;
    FILE *file;

    if (db_dir == NULL || table_name == NULL) {
        set_error(errbuf, errbuf_size, "invalid data file arguments");
        return STATUS_STORAGE_ERROR;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return STATUS_STORAGE_ERROR;
    }

    file = fopen(data_path, "ab");
    if (file == NULL) {
        set_error(errbuf, errbuf_size, "failed to open data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    fclose(file);
    free(data_path);
    return STATUS_OK;
}

int append_row_to_table_with_offset(const char *db_dir, const char *table_name,
                                    const Row *row, long *out_row_offset,
                                    char *errbuf, size_t errbuf_size)
{
    char *data_path;
    FILE *file;
    size_t i;
    int status = STATUS_OK;

    if (out_row_offset != NULL) {
        *out_row_offset = 0L;
    }

    if (db_dir == NULL || table_name == NULL || row == NULL || out_row_offset == NULL) {
        set_error(errbuf, errbuf_size, "invalid row append arguments");
        return STATUS_STORAGE_ERROR;
    }

    if (row->value_count == 0U) {
        set_error(errbuf, errbuf_size, "cannot append an empty row to table '%s'", table_name);
        return STATUS_STORAGE_ERROR;
    }

    status = ensure_table_data_file(db_dir, table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return STATUS_STORAGE_ERROR;
    }

    file = fopen(data_path, "ab+");
    if (file == NULL) {
        set_error(errbuf, errbuf_size, "failed to append data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        set_error(errbuf, errbuf_size, "failed to seek to end of '%s'", data_path);
        fclose(file);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    *out_row_offset = ftell(file);
    if (*out_row_offset < 0L) {
        set_error(errbuf, errbuf_size, "failed to determine row offset in '%s'", data_path);
        fclose(file);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    for (i = 0U; i < row->value_count; ++i) {
        char *escaped = escape_field(row->values[i]);

        if (escaped == NULL) {
            set_error(errbuf, errbuf_size, "failed to escape row value for table '%s'", table_name);
            status = STATUS_STORAGE_ERROR;
            break;
        }

        if ((i > 0U && fputc('|', file) == EOF) || fputs(escaped, file) == EOF) {
            set_error(errbuf, errbuf_size, "failed to write row data to '%s'", data_path);
            free(escaped);
            status = STATUS_STORAGE_ERROR;
            break;
        }

        free(escaped);
    }

    if (status == STATUS_OK && fputc('\n', file) == EOF) {
        set_error(errbuf, errbuf_size, "failed to terminate row in '%s'", data_path);
        status = STATUS_STORAGE_ERROR;
    }

    if (fclose(file) != 0 && status == STATUS_OK) {
        set_error(errbuf, errbuf_size, "failed to finalize data file '%s'", data_path);
        status = STATUS_STORAGE_ERROR;
    }

    free(data_path);
    return status;
}

int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size)
{
    long row_offset = 0L;

    return append_row_to_table_with_offset(db_dir, table_name, row, &row_offset, errbuf, errbuf_size);
}

int scan_table_rows_with_offsets(const char *db_dir, const char *table_name,
                                 size_t expected_column_count,
                                 RowScanCallback callback,
                                 void *user_data,
                                 char *errbuf, size_t errbuf_size)
{
    char *data_path;
    FILE *file;
    size_t line_number = 0U;

    if (db_dir == NULL || table_name == NULL || callback == NULL) {
        set_error(errbuf, errbuf_size, "invalid row scan arguments");
        return STATUS_STORAGE_ERROR;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return STATUS_STORAGE_ERROR;
    }

    file = fopen(data_path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            free(data_path);
            return STATUS_OK;
        }

        set_error(errbuf, errbuf_size, "failed to open data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    while (1) {
        Row row = {0};
        char *line = NULL;
        int read_status;
        long row_offset = ftell(file);
        int callback_status;

        read_status = read_line(file, &line);
        if (read_status == 0) {
            break;
        }
        if (read_status < 0 || row_offset < 0L) {
            set_error(errbuf, errbuf_size, "failed to read data file '%s'", data_path);
            fclose(file);
            free(data_path);
            return STATUS_STORAGE_ERROR;
        }

        line_number += 1U;
        if (parse_line_into_row(line, expected_column_count, &row) != 0) {
            set_error(errbuf, errbuf_size, "malformed row %zu in table '%s'", line_number, table_name);
            free(line);
            fclose(file);
            free(data_path);
            return STATUS_STORAGE_ERROR;
        }

        callback_status = callback(&row, row_offset, user_data, errbuf, errbuf_size);
        free_row(&row);
        free(line);

        if (callback_status == 1) {
            break;
        }
        if (callback_status < 0) {
            fclose(file);
            free(data_path);
            if (errbuf != NULL && errbuf_size > 0U && errbuf[0] == '\0') {
                set_error(errbuf, errbuf_size, "row scan callback failed for table '%s'", table_name);
            }
            return STATUS_STORAGE_ERROR;
        }
    }

    if (fclose(file) != 0) {
        set_error(errbuf, errbuf_size, "failed to close data file '%s'", data_path);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    free(data_path);
    return STATUS_OK;
}

int read_row_at_offset(const char *db_dir, const char *table_name,
                       long row_offset,
                       size_t expected_column_count,
                       Row *out_row,
                       char *errbuf, size_t errbuf_size)
{
    char *data_path;
    FILE *file;
    char *line = NULL;
    int read_status;

    if (out_row != NULL) {
        memset(out_row, 0, sizeof(*out_row));
    }

    if (db_dir == NULL || table_name == NULL || out_row == NULL || row_offset < 0L) {
        set_error(errbuf, errbuf_size, "invalid row offset read arguments");
        return STATUS_STORAGE_ERROR;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return STATUS_STORAGE_ERROR;
    }

    file = fopen(data_path, "rb");
    if (file == NULL) {
        set_error(errbuf, errbuf_size, "failed to open data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    if (fseek(file, row_offset, SEEK_SET) != 0) {
        set_error(errbuf, errbuf_size, "failed to seek to row offset %ld in '%s'", row_offset, data_path);
        fclose(file);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    read_status = read_line(file, &line);
    if (read_status <= 0) {
        set_error(errbuf, errbuf_size, "failed to read row at offset %ld in '%s'", row_offset, data_path);
        fclose(file);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    if (parse_line_into_row(line, expected_column_count, out_row) != 0) {
        set_error(errbuf, errbuf_size, "malformed row at offset %ld in table '%s'", row_offset, table_name);
        free(line);
        fclose(file);
        free(data_path);
        return STATUS_STORAGE_ERROR;
    }

    free(line);
    fclose(file);
    free(data_path);
    return STATUS_OK;
}

int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size)
{
    ReadAllRowsState state;
    int status;

    if (out_rows != NULL) {
        *out_rows = NULL;
    }
    if (out_row_count != NULL) {
        *out_row_count = 0U;
    }

    if (db_dir == NULL || table_name == NULL || out_rows == NULL || out_row_count == NULL) {
        set_error(errbuf, errbuf_size, "invalid row read arguments");
        return STATUS_STORAGE_ERROR;
    }

    state.rows = out_rows;
    state.row_count = out_row_count;

    status = scan_table_rows_with_offsets(db_dir, table_name, expected_column_count,
                                          read_all_rows_callback, &state,
                                          errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_rows(*out_rows, *out_row_count);
        *out_rows = NULL;
        *out_row_count = 0U;
        return status;
    }

    return STATUS_OK;
}

void free_row(Row *row)
{
    if (row == NULL) {
        return;
    }

    free_field_array(row->values, row->value_count);
    row->values = NULL;
    row->value_count = 0U;
}

void free_rows(Row *rows, size_t row_count)
{
    size_t i;

    if (rows == NULL) {
        return;
    }

    for (i = 0U; i < row_count; ++i) {
        free_row(&rows[i]);
    }

    free(rows);
}
