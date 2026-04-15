#ifndef RUNTIME_H
#define RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "bptree.h"
#include "schema.h"

typedef struct {
    char *table_name;
    TableSchema schema;
    int has_id_column;
    int id_column_index;
    int id_index_ready;
    uint64_t next_id;
    BPTree id_index;
} TableRuntime;

typedef struct {
    char *db_dir;
    TableRuntime *tables;
    size_t table_count;
    size_t table_capacity;
} ExecutionContext;

int init_execution_context(const char *db_dir,
                           ExecutionContext *out_ctx,
                           char *errbuf, size_t errbuf_size);

int get_or_load_table_runtime(ExecutionContext *ctx,
                              const char *table_name,
                              TableRuntime **out_table,
                              char *errbuf, size_t errbuf_size);

int build_id_index_for_table(const char *db_dir,
                             const TableSchema *schema,
                             int id_column_index,
                             BPTree *out_tree,
                             uint64_t *out_next_id,
                             char *errbuf, size_t errbuf_size);

int parse_stored_id_value(const char *text,
                          uint64_t *out_id,
                          char *errbuf, size_t errbuf_size);

int try_parse_indexable_id_literal(const LiteralValue *literal,
                                   uint64_t *out_id);

void free_execution_context(ExecutionContext *ctx);

#endif
