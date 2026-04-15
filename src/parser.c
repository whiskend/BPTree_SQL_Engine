#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

enum {
    STATUS_OK = 0,
    STATUS_PARSE_ERROR = 4
};

typedef struct {
    const TokenArray *tokens;
    size_t current;
} Parser;

static void set_error(char *errbuf, size_t errbuf_size, const char *message) {
    if (errbuf != NULL && errbuf_size > 0) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

static void free_string_list(char **items, size_t count) {
    size_t i;

    if (items == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static void free_literal_list(LiteralValue *items, size_t count) {
    size_t i;

    if (items == NULL) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(items[i].text);
    }
    free(items);
}

static const Token *parser_peek(Parser *parser) {
    return &parser->tokens->items[parser->current];
}

static const Token *parser_previous(Parser *parser) {
    return &parser->tokens->items[parser->current - 1];
}

static const Token *parser_advance(Parser *parser) {
    if (parser_peek(parser)->type != TOKEN_EOF) {
        ++parser->current;
    }
    return parser_previous(parser);
}

static int parser_match(Parser *parser, TokenType type) {
    if (parser_peek(parser)->type != type) {
        return 0;
    }
    parser_advance(parser);
    return 1;
}

static void parser_skip_semicolons(Parser *parser) {
    while (parser_match(parser, TOKEN_SEMICOLON)) {
        /* empty statement separators are ignored */
    }
}


static int parser_expect_with_message(Parser *parser, TokenType type, const char *message, char *errbuf, size_t errbuf_size) {
    if (parser_match(parser, type)) {
        return STATUS_OK;
    }

    set_error(errbuf, errbuf_size, message);
    return STATUS_PARSE_ERROR;
}

static int append_identifier(char ***out_items, size_t *out_count, const char *text) {
    char **grown = (char **)realloc(*out_items, sizeof(char *) * (*out_count + 1));
    if (grown == NULL) {
        return 0;
    }

    *out_items = grown;
    (*out_items)[*out_count] = strdup_safe(text);
    if ((*out_items)[*out_count] == NULL) {
        return 0;
    }
    ++(*out_count);
    return 1;
}

static int append_literal(LiteralValue **out_items, size_t *out_count, ValueType type, const char *text) {
    LiteralValue *grown = (LiteralValue *)realloc(*out_items, sizeof(LiteralValue) * (*out_count + 1));
    if (grown == NULL) {
        return 0;
    }

    *out_items = grown;
    (*out_items)[*out_count].type = type;
    (*out_items)[*out_count].text = strdup_safe(text);
    if ((*out_items)[*out_count].text == NULL) {
        return 0;
    }
    ++(*out_count);
    return 1;
}

static int parse_identifier(Parser *parser, char **out_text, char *errbuf, size_t errbuf_size) {
    if (parser_peek(parser)->type != TOKEN_IDENTIFIER) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected identifier");
        return STATUS_PARSE_ERROR;
    }

    *out_text = strdup_safe(parser_advance(parser)->text);
    if (*out_text == NULL) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    return STATUS_OK;
}

static int parse_literal(Parser *parser, LiteralValue *out_value, char *errbuf, size_t errbuf_size) {
    const Token *token = parser_peek(parser);

    if (token->type == TOKEN_STRING) {
        out_value->type = VALUE_STRING;
        out_value->text = strdup_safe(token->text);
        if (out_value->text == NULL) {
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        parser_advance(parser);
        return STATUS_OK;
    }

    if (token->type == TOKEN_NUMBER) {
        out_value->type = VALUE_NUMBER;
        out_value->text = strdup_safe(token->text);
        if (out_value->text == NULL) {
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        parser_advance(parser);
        return STATUS_OK;
    }

    set_error(errbuf, errbuf_size, "PARSE ERROR: expected literal");
    return STATUS_PARSE_ERROR;
}

static int parse_identifier_list(Parser *parser, char ***out_items, size_t *out_count,
                                 char *errbuf, size_t errbuf_size) {
    char *identifier = NULL;
    int status;

    *out_items = NULL;
    *out_count = 0;

    status = parse_identifier(parser, &identifier, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (!append_identifier(out_items, out_count, identifier)) {
        free(identifier);
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    free(identifier);

    while (parser_match(parser, TOKEN_COMMA)) {
        status = parse_identifier(parser, &identifier, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_string_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            return status;
        }

        if (!append_identifier(out_items, out_count, identifier)) {
            free(identifier);
            free_string_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        free(identifier);
    }

    return STATUS_OK;
}

static int parse_literal_list(Parser *parser, LiteralValue **out_items, size_t *out_count,
                              char *errbuf, size_t errbuf_size) {
    LiteralValue literal;
    int status;

    *out_items = NULL;
    *out_count = 0;

    literal.type = VALUE_STRING;
    literal.text = NULL;
    status = parse_literal(parser, &literal, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (!append_literal(out_items, out_count, literal.type, literal.text)) {
        free(literal.text);
        set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
        return STATUS_PARSE_ERROR;
    }
    free(literal.text);

    while (parser_match(parser, TOKEN_COMMA)) {
        literal.type = VALUE_STRING;
        literal.text = NULL;
        status = parse_literal(parser, &literal, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_literal_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            return status;
        }

        if (!append_literal(out_items, out_count, literal.type, literal.text)) {
            free(literal.text);
            free_literal_list(*out_items, *out_count);
            *out_items = NULL;
            *out_count = 0;
            set_error(errbuf, errbuf_size, "PARSE ERROR: out of memory");
            return STATUS_PARSE_ERROR;
        }
        free(literal.text);
    }

    return STATUS_OK;
}

static int parse_where_clause(Parser *parser, WhereClause *out_clause, char *errbuf, size_t errbuf_size) {
    int status;

    memset(out_clause, 0, sizeof(*out_clause));

    if (!parser_match(parser, TOKEN_WHERE)) {
        return STATUS_OK;
    }

    out_clause->has_condition = 1;

    status = parse_identifier(parser, &out_clause->column_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_EQUAL,
                                        "PARSE ERROR: expected '=' in WHERE clause",
                                        errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parse_literal(parser, &out_clause->value, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    return STATUS_OK;
}

static int parse_insert(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    InsertStatement *stmt = &out_stmt->insert_stmt;
    int status;

    memset(stmt, 0, sizeof(*stmt));

    status = parser_expect_with_message(parser, TOKEN_INTO, "PARSE ERROR: expected INTO after INSERT", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    status = parse_identifier(parser, &stmt->table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (parser_match(parser, TOKEN_LPAREN)) {
        status = parse_identifier_list(parser, &stmt->columns, &stmt->column_count, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_statement(out_stmt);
            return status;
        }

        status = parser_expect_with_message(parser, TOKEN_RPAREN, "PARSE ERROR: expected ')' after column list", errbuf, errbuf_size);
        if (status != STATUS_OK) {
            free_statement(out_stmt);
            return status;
        }
    }

    status = parser_expect_with_message(parser, TOKEN_VALUES, "PARSE ERROR: expected VALUES in INSERT statement", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_LPAREN, "PARSE ERROR: expected '(' before value list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_literal_list(parser, &stmt->values, &stmt->value_count, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parser_expect_with_message(parser, TOKEN_RPAREN, "PARSE ERROR: expected ')' after value list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    out_stmt->type = STMT_INSERT;
    return STATUS_OK;
}

static int parse_select(Parser *parser, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    SelectStatement *stmt = &out_stmt->select_stmt;
    int status;

    memset(stmt, 0, sizeof(*stmt));

    if (parser_match(parser, TOKEN_STAR)) {
        stmt->select_all = 1;
    } else {
        status = parse_identifier_list(parser, &stmt->columns, &stmt->column_count, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            return status;
        }
    }

    status = parser_expect_with_message(parser, TOKEN_FROM, "PARSE ERROR: expected FROM after select list", errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_identifier(parser, &stmt->table_name, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    status = parse_where_clause(parser, &stmt->where_clause, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        free_statement(out_stmt);
        return status;
    }

    out_stmt->type = STMT_SELECT;
    return STATUS_OK;
}

int parse_next_statement(const TokenArray *tokens, size_t *cursor,
                         Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    Parser parser;
    int status;

    if (tokens == NULL || cursor == NULL || out_stmt == NULL || tokens->count == 0 || tokens->items == NULL) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: invalid token stream");
        return STATUS_PARSE_ERROR;
    }

    if (*cursor >= tokens->count) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: parser cursor out of range");
        return STATUS_PARSE_ERROR;
    }

    if (errbuf != NULL && errbuf_size > 0) {
        errbuf[0] = '\0';
    }

    memset(out_stmt, 0, sizeof(*out_stmt));
    parser.tokens = tokens;
    parser.current = *cursor;
    parser_skip_semicolons(&parser);

    if (parser_peek(&parser)->type == TOKEN_EOF) {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected INSERT or SELECT");
        *cursor = parser.current;
        return STATUS_PARSE_ERROR;
    }

    if (parser_match(&parser, TOKEN_INSERT)) {
        out_stmt->type = STMT_INSERT;
        status = parse_insert(&parser, out_stmt, errbuf, errbuf_size);
    } else if (parser_match(&parser, TOKEN_SELECT)) {
        out_stmt->type = STMT_SELECT;
        status = parse_select(&parser, out_stmt, errbuf, errbuf_size);
    } else {
        set_error(errbuf, errbuf_size, "PARSE ERROR: expected INSERT or SELECT");
        return STATUS_PARSE_ERROR;
    }

    if (status != STATUS_OK) {
        return status;
    }

    if (parser_match(&parser, TOKEN_SEMICOLON)) {
        /* statement terminator is optional */
    }

    *cursor = parser.current;
    return STATUS_OK;
}

int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size) {
    size_t cursor = 0;
    int status;

    status = parse_next_statement(tokens, &cursor, out_stmt, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        ++cursor;
    }

    if (cursor >= tokens->count || tokens->items[cursor].type != TOKEN_EOF) {
        free_statement(out_stmt);
        set_error(errbuf, errbuf_size, "PARSE ERROR: unexpected trailing tokens");
        return STATUS_PARSE_ERROR;
    }

    return STATUS_OK;
}

void free_statement(Statement *stmt) {
    if (stmt == NULL) {
        return;
    }

    if (stmt->type == STMT_INSERT) {
        free(stmt->insert_stmt.table_name);
        free_string_list(stmt->insert_stmt.columns, stmt->insert_stmt.column_count);
        free_literal_list(stmt->insert_stmt.values, stmt->insert_stmt.value_count);
        stmt->insert_stmt.table_name = NULL;
        stmt->insert_stmt.columns = NULL;
        stmt->insert_stmt.column_count = 0;
        stmt->insert_stmt.values = NULL;
        stmt->insert_stmt.value_count = 0;
    } else if (stmt->type == STMT_SELECT) {
        free(stmt->select_stmt.table_name);
        free_string_list(stmt->select_stmt.columns, stmt->select_stmt.column_count);
        free(stmt->select_stmt.where_clause.column_name);
        free(stmt->select_stmt.where_clause.value.text);
        stmt->select_stmt.table_name = NULL;
        stmt->select_stmt.columns = NULL;
        stmt->select_stmt.column_count = 0;
        stmt->select_stmt.select_all = 0;
        stmt->select_stmt.where_clause.has_condition = 0;
        stmt->select_stmt.where_clause.column_name = NULL;
        stmt->select_stmt.where_clause.value.type = VALUE_STRING;
        stmt->select_stmt.where_clause.value.text = NULL;
    }
}

