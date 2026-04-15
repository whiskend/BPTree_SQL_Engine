#include "storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...) {
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

static char *dup_string(const char *value) {
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

static char *build_table_path(const char *db_dir, const char *table_name, const char *suffix) {
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

static int read_line(FILE *file, char **out_line) {
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

static int append_char(char **buffer, size_t *length, size_t *capacity, char ch) {
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

static int push_field(char ***fields, size_t *field_count, char *field_value) {
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

static void free_field_array(char **fields, size_t field_count) {
    size_t i;

    if (fields == NULL) {
        return;
    }

    for (i = 0U; i < field_count; i++) {
        free(fields[i]);
    }
    free(fields);
}

static int normalize_field_count(char ***fields, size_t *field_count, size_t expected_count) {
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

static char *escape_field(const char *value) {
    const char *src;
    size_t out_len;
    char *escaped;
    char *dst;

    src = (value == NULL) ? "" : value;
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

    src = (value == NULL) ? "" : value;
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

static char *unescape_field(const char *value) {
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
    for (src_index = 0U; src_index < len; src_index++) {
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

static int split_escaped_row(const char *line, char ***out_fields, size_t *out_count) {
    char **fields;
    size_t field_count;
    char *current_raw;
    size_t current_len;
    size_t current_capacity;
    int escape_pending;
    const char *cursor;

    if (line == NULL || out_fields == NULL || out_count == NULL) {
        return 1;
    }

    *out_fields = NULL;
    *out_count = 0U;
    fields = NULL;
    field_count = 0U;
    current_raw = NULL;
    current_len = 0U;
    current_capacity = 0U;
    escape_pending = 0;

    for (cursor = line; *cursor != '\0'; cursor++) {
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
        char *field_value;

        field_value = unescape_field(current_raw);
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

int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size) {
    char *data_path;
    FILE *file;

    if (db_dir == NULL || table_name == NULL) {
        set_error(errbuf, errbuf_size, "invalid data file arguments");
        return 1;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return 1;
    }

    file = fopen(data_path, "ab");
    if (file == NULL) {
        set_error(errbuf, errbuf_size, "failed to open data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return 1;
    }

    fclose(file);
    free(data_path);
    return 0;
}

int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size) {
    char *data_path;
    FILE *file;
    size_t i;
    int status;

    if (db_dir == NULL || table_name == NULL || row == NULL) {
        set_error(errbuf, errbuf_size, "invalid row append arguments");
        return 1;
    }

    if (row->value_count == 0U) {
        set_error(errbuf, errbuf_size, "cannot append an empty row to table '%s'", table_name);
        return 1;
    }

    if (ensure_table_data_file(db_dir, table_name, errbuf, errbuf_size) != 0) {
        return 1;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return 1;
    }

    file = fopen(data_path, "ab");
    if (file == NULL) {
        set_error(errbuf, errbuf_size, "failed to append data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return 1;
    }

    status = 0;
    for (i = 0U; i < row->value_count; i++) {
        char *escaped;

        escaped = escape_field(row->values[i]);
        if (escaped == NULL) {
            set_error(errbuf, errbuf_size, "failed to escape row value for table '%s'", table_name);
            status = 1;
            break;
        }

        if ((i > 0U && fputc('|', file) == EOF) || fputs(escaped, file) == EOF) {
            set_error(errbuf, errbuf_size, "failed to write row data to '%s'", data_path);
            free(escaped);
            status = 1;
            break;
        }

        free(escaped);
    }

    if (status == 0 && fputc('\n', file) == EOF) {
        set_error(errbuf, errbuf_size, "failed to terminate row in '%s'", data_path);
        status = 1;
    }

    if (fclose(file) != 0 && status == 0) {
        set_error(errbuf, errbuf_size, "failed to finalize data file '%s'", data_path);
        status = 1;
    }

    free(data_path);
    return status;
}

int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size) {
    char *data_path;
    FILE *file;
    Row *rows;
    size_t row_count;
    int status;
    size_t line_number;

    if (out_rows != NULL) {
        *out_rows = NULL;
    }
    if (out_row_count != NULL) {
        *out_row_count = 0U;
    }

    if (db_dir == NULL || table_name == NULL || out_rows == NULL || out_row_count == NULL) {
        set_error(errbuf, errbuf_size, "invalid row read arguments");
        return 1;
    }

    data_path = build_table_path(db_dir, table_name, ".data");
    if (data_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate data path for table '%s'", table_name);
        return 1;
    }

    file = fopen(data_path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            free(data_path);
            return 0;
        }

        set_error(errbuf, errbuf_size, "failed to open data file '%s': %s", data_path, strerror(errno));
        free(data_path);
        return 1;
    }

    rows = NULL;
    row_count = 0U;
    status = 0;
    line_number = 0U;

    while (1) {
        char *line;
        int read_status;

        read_status = read_line(file, &line);
        if (read_status == 0) {
            break;
        }
        if (read_status < 0) {
            set_error(errbuf, errbuf_size, "failed to read data file '%s'", data_path);
            status = 1;
            break;
        }

        line_number++;

        {
            char **fields;
            size_t field_count;
            Row *grown_rows;

            if (split_escaped_row(line, &fields, &field_count) != 0) {
                set_error(errbuf, errbuf_size, "malformed row %zu in table '%s'", line_number, table_name);
                free(line);
                status = 1;
                break;
            }

            if (normalize_field_count(&fields, &field_count, expected_column_count) != 0) {
                set_error(errbuf, errbuf_size,
                          "failed to align row %zu in table '%s' with current schema",
                          line_number, table_name);
                free(line);
                status = 1;
                break;
            }

            grown_rows = (Row *)realloc(rows, (row_count + 1U) * sizeof(Row));
            if (grown_rows == NULL) {
                set_error(errbuf, errbuf_size, "failed to allocate row array for table '%s'", table_name);
                free_field_array(fields, field_count);
                free(line);
                status = 1;
                break;
            }

            rows = grown_rows;
            rows[row_count].values = fields;
            rows[row_count].value_count = field_count;
            row_count += 1U;
        }

        free(line);
    }

    if (fclose(file) != 0 && status == 0) {
        set_error(errbuf, errbuf_size, "failed to close data file '%s'", data_path);
        status = 1;
    }

    free(data_path);

    if (status != 0) {
        free_rows(rows, row_count);
        return 1;
    }

    *out_rows = rows;
    *out_row_count = row_count;
    return 0;
}

void free_rows(Row *rows, size_t row_count) {
    size_t i;

    if (rows == NULL) {
        return;
    }

    for (i = 0U; i < row_count; i++) {
        free_field_array(rows[i].values, rows[i].value_count);
        rows[i].values = NULL;
        rows[i].value_count = 0U;
    }

    free(rows);
}
