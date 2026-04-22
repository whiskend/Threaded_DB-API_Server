#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

#include "ast.h"
#include "lexer.h"

/* tokens 전체를 단일 SQL 문으로 파싱해 out_stmt에 AST를 채우고 상태 코드를 반환한다. */
int parse_statement(const TokenArray *tokens, Statement *out_stmt, char *errbuf, size_t errbuf_size);
/*
 * tokens와 cursor를 받아 cursor 이후 다음 SQL 문 하나만 파싱하고,
 * out_stmt에 AST를 채운 뒤 cursor를 다음 문장 시작으로 이동시킨다.
 */
int parse_next_statement(const TokenArray *tokens, size_t *cursor,
                         Statement *out_stmt, char *errbuf, size_t errbuf_size);
/* Statement 내부에 할당된 문자열/배열 메모리를 모두 해제한다. */
void free_statement(Statement *stmt);

#endif
