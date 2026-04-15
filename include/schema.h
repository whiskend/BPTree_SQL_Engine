#ifndef SCHEMA_H
#define SCHEMA_H

#include <stddef.h>

typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
} TableSchema;

typedef struct {
    char **values;
    size_t value_count;
} Row;

int load_table_schema(const char *db_dir, const char *table_name,
                      TableSchema *out_schema,
                      char *errbuf, size_t errbuf_size);

int schema_find_column_index(const TableSchema *schema, const char *column_name);
void free_table_schema(TableSchema *schema);

#endif
