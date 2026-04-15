#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

#include "ast.h"
#include "lexer.h"

int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
int parse_next_statement(const TokenArray *tokens, size_t *cursor,
                         Statement *out_stmt, char *errbuf, size_t errbuf_size);
void free_statement(Statement *stmt);

#endif
