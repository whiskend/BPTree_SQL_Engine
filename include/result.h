#ifndef RESULT_H
#define RESULT_H

#include <stddef.h>

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
} ExecResult;

void print_exec_result(const ExecResult *result);

#endif
