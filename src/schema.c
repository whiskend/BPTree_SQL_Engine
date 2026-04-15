#include "schema.h"

#include <ctype.h>
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

static char *duplicate_string(const char *src) {
    size_t len;
    char *copy;

    if (src == NULL) {
        return NULL;
    }

    len = strlen(src) + 1U;
    copy = (char *)malloc(len);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, src, len);
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

static void trim_in_place(char *text) {
    char *start;
    char *end;
    size_t trimmed_len;

    if (text == NULL) {
        return;
    }

    start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    trimmed_len = (size_t)(end - start);
    if (start != text) {
        memmove(text, start, trimmed_len);
    }
    text[trimmed_len] = '\0';
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

static void free_string_array(char **items, size_t count) {
    size_t i;

    if (items == NULL) {
        return;
    }

    for (i = 0U; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int append_column(char ***columns, size_t *column_count, const char *column_name) {
    char *copy;
    char **grown;

    copy = duplicate_string(column_name);
    if (copy == NULL) {
        return 1;
    }

    grown = (char **)realloc(*columns, (*column_count + 1U) * sizeof(char *));
    if (grown == NULL) {
        free(copy);
        return 1;
    }

    grown[*column_count] = copy;
    *columns = grown;
    *column_count += 1U;
    return 0;
}

int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size) {
    char *schema_path;
    FILE *file;
    char **columns;
    size_t column_count;
    int read_status;
    int status;

    if (out_schema != NULL) {
        out_schema->table_name = NULL;
        out_schema->columns = NULL;
        out_schema->column_count = 0U;
    }

    if (db_dir == NULL || table_name == NULL || out_schema == NULL) {
        set_error(errbuf, errbuf_size, "invalid schema load arguments");
        return 1;
    }

    schema_path = build_table_path(db_dir, table_name, ".schema");
    if (schema_path == NULL) {
        set_error(errbuf, errbuf_size, "failed to allocate schema path for table '%s'", table_name);
        return 1;
    }

    file = fopen(schema_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            set_error(errbuf, errbuf_size, "table '%s' not found", table_name);
        } else {
            set_error(errbuf, errbuf_size, "failed to open schema file '%s': %s", schema_path, strerror(errno));
        }
        free(schema_path);
        return 1;
    }

    columns = NULL;
    column_count = 0U;
    status = 0;

    while (1) {
        char *line;
        size_t i;

        read_status = read_line(file, &line);
        if (read_status == 0) {
            break;
        }
        if (read_status < 0) {
            set_error(errbuf, errbuf_size, "failed to read schema file '%s'", schema_path);
            status = 1;
            break;
        }

        trim_in_place(line);
        if (line[0] == '\0') {
            free(line);
            continue;
        }

        for (i = 0U; i < column_count; i++) {
            if (strcmp(columns[i], line) == 0) {
                set_error(errbuf, errbuf_size, "duplicate column '%s' in table '%s'", line, table_name);
                status = 1;
                break;
            }
        }

        if (status == 0 && append_column(&columns, &column_count, line) != 0) {
            set_error(errbuf, errbuf_size, "failed to allocate schema column '%s'", line);
            status = 1;
        }

        free(line);
        if (status != 0) {
            break;
        }
    }

    fclose(file);
    free(schema_path);

    if (status == 0 && column_count == 0U) {
        set_error(errbuf, errbuf_size, "table '%s' has no columns", table_name);
        status = 1;
    }

    if (status != 0) {
        free_string_array(columns, column_count);
        return 1;
    }

    out_schema->table_name = duplicate_string(table_name);
    if (out_schema->table_name == NULL) {
        free_string_array(columns, column_count);
        set_error(errbuf, errbuf_size, "failed to allocate schema table name '%s'", table_name);
        return 1;
    }

    out_schema->columns = columns;
    out_schema->column_count = column_count;
    return 0;
}

int schema_find_column_index(const TableSchema *schema, const char *column_name) {
    size_t i;

    if (schema == NULL || column_name == NULL) {
        return -1;
    }

    for (i = 0U; i < schema->column_count; i++) {
        if (schema->columns[i] != NULL && strcmp(schema->columns[i], column_name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

void free_table_schema(TableSchema *schema) {
    if (schema == NULL) {
        return;
    }

    free(schema->table_name);
    free_string_array(schema->columns, schema->column_count);
    schema->table_name = NULL;
    schema->columns = NULL;
    schema->column_count = 0U;
}
