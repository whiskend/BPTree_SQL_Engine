#ifndef AST_H
#define AST_H

#include <stddef.h>

typedef enum {
    VALUE_STRING,
    VALUE_NUMBER
} ValueType;

typedef struct {
    ValueType type;
    char *text;
} LiteralValue;

typedef struct {
    char *table_name;
    char **columns;
    size_t column_count;
    LiteralValue *values;
    size_t value_count;
} InsertStatement;

typedef struct {
    int has_condition;
    char *column_name;
    LiteralValue value;
} WhereClause;

typedef struct {
    char *table_name;
    int select_all;
    char **columns;
    size_t column_count;
    WhereClause where_clause;
} SelectStatement;

typedef enum {
    STMT_INSERT,
    STMT_SELECT
} StatementType;

typedef struct {
    StatementType type;
    union {
        InsertStatement insert_stmt;
        SelectStatement select_stmt;
    };
} Statement;

#endif
