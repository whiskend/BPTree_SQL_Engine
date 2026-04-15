#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

#include "schema.h"

typedef int (*RowScanCallback)(const Row *row,
                               long row_offset,
                               void *user_data,
                               char *errbuf,
                               size_t errbuf_size);

int ensure_table_data_file(const char *db_dir, const char *table_name,
                           char *errbuf, size_t errbuf_size);

int append_row_to_table_with_offset(const char *db_dir, const char *table_name,
                                    const Row *row, long *out_row_offset,
                                    char *errbuf, size_t errbuf_size);

int append_row_to_table(const char *db_dir, const char *table_name,
                        const Row *row,
                        char *errbuf, size_t errbuf_size);

int scan_table_rows_with_offsets(const char *db_dir, const char *table_name,
                                 size_t expected_column_count,
                                 RowScanCallback callback,
                                 void *user_data,
                                 char *errbuf, size_t errbuf_size);

int read_row_at_offset(const char *db_dir, const char *table_name,
                       long row_offset,
                       size_t expected_column_count,
                       Row *out_row,
                       char *errbuf, size_t errbuf_size);

int read_all_rows_from_table(const char *db_dir, const char *table_name,
                             size_t expected_column_count,
                             Row **out_rows, size_t *out_row_count,
                             char *errbuf, size_t errbuf_size);

void free_row(Row *row);
void free_rows(Row *rows, size_t row_count);

#endif
