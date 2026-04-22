#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "ast.h"
#include "result.h"
#include "runtime.h"

/*
 * ctx가 들고 있는 runtime cache를 사용해 stmt 하나를 실행하고,
 * 결과를 out_result에 채운 뒤 STATUS_* 코드를 반환한다.
 */
int execute_statement(ExecutionContext *ctx, const Statement *stmt,
                      ExecResult *out_result,
                      char *errbuf, size_t errbuf_size);

/* ExecResult 내부의 query_result 메모리를 해제하고 구조체를 재사용 가능한 상태로 만든다. */
void free_exec_result(ExecResult *result);

#endif
