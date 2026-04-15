#ifndef RESULT_H
#define RESULT_H

#include <stddef.h>
#include <stdint.h>

#include "schema.h"

typedef enum {
    RESULT_INSERT,
    RESULT_SELECT
} ExecResultType;

typedef struct {
    char **columns;
    size_t column_count;
    Row *rows;
    size_t row_count;
} QueryResult;

typedef struct {
    ExecResultType type;
    size_t affected_rows;
    QueryResult query_result;
    int used_index;
    int has_generated_id;
    uint64_t generated_id;
} ExecResult;

void print_exec_result(const ExecResult *result);

#endif
