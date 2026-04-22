#!/bin/sh

set -eu

# 테스트가 실행되는 저장소 루트 절대 경로다.
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
# 실제 SQL 처리기를 실행할 바이너리 경로로, 환경변수 override가 가능하다.
BIN_PATH="${SQL_PROCESSOR_BIN:-$ROOT_DIR/sql_processor}"
# schema/data/sql/output fixture를 만들 임시 디렉터리다.
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sql-processor.XXXXXX")

# 테스트 종료 시 임시 디렉터리를 제거해 흔적을 남기지 않는다.
cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

mkdir -p "$TMP_DIR/db"

## users 테이블 schema fixture를 만들어 auto-id와 조회 통합 시나리오를 준비한다.
cat > "$TMP_DIR/db/users.schema" <<'EOF'
id
name
age
EOF

## 첫 번째 SQL 파일을 실행해 초기 데이터 삽입 경로를 검증한다.
"$BIN_PATH" -d "$TMP_DIR/db" -f "$ROOT_DIR/queries/insert_users.sql" > "$TMP_DIR/insert1.out"

## 추가 INSERT와 전체 조회, id 조회를 한 배치로 실행해 multi-statement 흐름을 검증한다.
cat > "$TMP_DIR/batch.sql" <<'EOF'
INSERT INTO users VALUES ('Bob', 25);
SELECT * FROM users;
SELECT id, name FROM users WHERE id = 2;
EOF

## 배치 SQL을 실행해 INSERT 후 전체 SELECT와 id SELECT가 모두 정상 출력되는지 확인한다.
"$BIN_PATH" -d "$TMP_DIR/db" -f "$TMP_DIR/batch.sql" > "$TMP_DIR/batch.out"

## auto-generated id가 실제 data 파일에 기록된 기대 결과다.
cat > "$TMP_DIR/expected.data" <<'EOF'
1|Alice|20
2|Bob|25
EOF

## data 파일 내용과 CLI 출력이 기대한 결과와 일치하는지 검증한다.
cmp "$TMP_DIR/db/users.data" "$TMP_DIR/expected.data"
grep -q "INSERT 1" "$TMP_DIR/insert1.out"
grep -q "INSERT 1" "$TMP_DIR/batch.out"
grep -q "Alice" "$TMP_DIR/batch.out"
grep -q "Bob" "$TMP_DIR/batch.out"
grep -q "2 rows selected" "$TMP_DIR/batch.out"
grep -q "1 rows selected" "$TMP_DIR/batch.out"

## 모든 검증이 끝나면 통합 테스트 성공 메시지를 출력한다.
echo "integration: OK"
