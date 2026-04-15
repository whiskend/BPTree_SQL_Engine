#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "ast.h"
#include "result.h"
#include "runtime.h"

int execute_statement(ExecutionContext *ctx, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

void free_exec_result(ExecResult *result);

#endif
