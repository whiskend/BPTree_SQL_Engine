#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "ast.h"
#include "result.h"

int execute_statement(const char *db_dir, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

void free_exec_result(ExecResult *result);

#endif
