#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mini-db-server-test.XXXXXX")
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

get_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_for_server() {
    port=$1
    attempts=0
    while [ "$attempts" -lt 30 ]; do
        if curl -s "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 1
    done
    return 1
}

start_server() {
    db_dir=$1
    port=$2
    threads=$3
    queue_size=$4
    log_file=$5

    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
        SERVER_PID=""
    fi

    "$BIN_PATH" -d "$db_dir" -p "$port" -t "$threads" -q "$queue_size" >"$log_file" 2>&1 &
    SERVER_PID=$!

    if ! wait_for_server "$port"; then
        cat "$log_file" >&2
        return 1
    fi

    return 0
}

curl_json() {
    method=$1
    url=$2
    data=$3
    out_file=$4

    if [ -n "$data" ]; then
        curl --max-time 20 -sS -o "$out_file" -w "%{http_code}" \
            -X "$method" "$url" \
            -H "Content-Type: application/json" \
            --data "$data"
    else
        curl --max-time 20 -sS -o "$out_file" -w "%{http_code}" \
            -X "$method" "$url"
    fi
}

curl_json_allow_error() {
    method=$1
    url=$2
    data=$3
    out_file=$4

    if [ -n "$data" ]; then
        (
            curl --max-time 20 -sS -o "$out_file" -w "%{http_code}" \
                -X "$method" "$url" \
                -H "Content-Type: application/json" \
                --data "$data"
        ) 2>/dev/null || true
    else
        (
            curl --max-time 20 -sS -o "$out_file" -w "%{http_code}" \
                -X "$method" "$url"
        ) 2>/dev/null || true
    fi
}

assert_code() {
    expected=$1
    actual=$2
    if [ "$expected" != "$actual" ]; then
        echo "expected HTTP $expected but got $actual" >&2
        exit 1
    fi
}

assert_jq() {
    file=$1
    expr=$2
    jq -e "$expr" "$file" >/dev/null
}

mkdir -p "$TMP_DIR/db"
cat > "$TMP_DIR/db/users.schema" <<'EOF'
id
name
age
EOF

PORT=$(get_free_port)
LOG_FILE="$TMP_DIR/server.log"
BASE_URL="http://127.0.0.1:$PORT"

start_server "$TMP_DIR/db" "$PORT" 4 64 "$LOG_FILE"

STATUS=$(curl_json GET "$BASE_URL/health" "" "$TMP_DIR/health.json")
assert_code 200 "$STATUS"
assert_jq "$TMP_DIR/health.json" '.success == true and .service == "mini_db_server"'

STATUS=$(curl_json POST "$BASE_URL/query" "{\"sql\":\"INSERT INTO users VALUES ('Alice', 20);\"}" "$TMP_DIR/insert_one.json")
assert_code 200 "$STATUS"
assert_jq "$TMP_DIR/insert_one.json" '.success == true and .type == "insert" and .affected_rows == 1 and .generated_id == 1'

STATUS=$(curl_json POST "$BASE_URL/query" "{\"sql\":\"SELECT * FROM users WHERE id = 1;\"}" "$TMP_DIR/select_one.json")
assert_code 200 "$STATUS"
assert_jq "$TMP_DIR/select_one.json" '.success == true and .type == "select" and .used_index == true and .row_count == 1 and .rows[0][1] == "Alice"'

STATUS=$(curl_json POST "$BASE_URL/query" '{"sql":' "$TMP_DIR/invalid_json.json")
assert_code 400 "$STATUS"
assert_jq "$TMP_DIR/invalid_json.json" '.success == false and .error_code == "INVALID_JSON"'

STATUS=$(curl_json POST "$BASE_URL/query" '{"foo":"bar"}' "$TMP_DIR/missing_sql.json")
assert_code 400 "$STATUS"
assert_jq "$TMP_DIR/missing_sql.json" '.success == false and .error_code == "MISSING_SQL_FIELD"'

STATUS=$(curl_json POST "$BASE_URL/query" '{"sql":"   ; ;   "}' "$TMP_DIR/empty_sql.json")
assert_code 400 "$STATUS"
assert_jq "$TMP_DIR/empty_sql.json" '.success == false and .error_code == "EMPTY_QUERY"'

STATUS=$(curl_json POST "$BASE_URL/query" "{\"sql\":\"SELECT * FROM users; SELECT * FROM users;\"}" "$TMP_DIR/multi.sql.json")
assert_code 400 "$STATUS"
assert_jq "$TMP_DIR/multi.sql.json" '.success == false and .error_code == "MULTI_STATEMENT_NOT_ALLOWED"'

STATUS=$(curl_json GET "$BASE_URL/not-found" "" "$TMP_DIR/not_found.json")
assert_code 404 "$STATUS"
assert_jq "$TMP_DIR/not_found.json" '.success == false and .error_code == "NOT_FOUND"'

STATUS=$(curl_json GET "$BASE_URL/query" "" "$TMP_DIR/method_not_allowed.json")
assert_code 405 "$STATUS"
assert_jq "$TMP_DIR/method_not_allowed.json" '.success == false and .error_code == "METHOD_NOT_ALLOWED"'

i=1
pids=""
while [ "$i" -le 12 ]; do
    (
        status=$(curl_json POST "$BASE_URL/query" "{\"sql\":\"SELECT * FROM users WHERE id = 1;\"}" "$TMP_DIR/select_burst_$i.json")
        printf '%s' "$status" > "$TMP_DIR/select_burst_$i.status"
    ) &
    pids="$pids $!"
    i=$((i + 1))
done
for pid in $pids; do
    wait "$pid"
done

i=1
while [ "$i" -le 12 ]; do
    STATUS=$(cat "$TMP_DIR/select_burst_$i.status")
    assert_code 200 "$STATUS"
    assert_jq "$TMP_DIR/select_burst_$i.json" '.success == true and .type == "select" and .row_count == 1'
    i=$((i + 1))
done

i=1
pids=""
while [ "$i" -le 12 ]; do
    (
        payload="{\"sql\":\"INSERT INTO users VALUES ('user_$i', $((20 + i)));\"}"
        status=$(curl_json POST "$BASE_URL/query" "$payload" "$TMP_DIR/insert_burst_$i.json")
        printf '%s' "$status" > "$TMP_DIR/insert_burst_$i.status"
    ) &
    pids="$pids $!"
    i=$((i + 1))
done
for pid in $pids; do
    wait "$pid"
done

i=1
while [ "$i" -le 12 ]; do
    STATUS=$(cat "$TMP_DIR/insert_burst_$i.status")
    assert_code 200 "$STATUS"
    assert_jq "$TMP_DIR/insert_burst_$i.json" '.success == true and .type == "insert"'
    i=$((i + 1))
done

i=1
while [ "$i" -le 12 ]; do
    jq -r '.generated_id' "$TMP_DIR/insert_burst_$i.json"
    i=$((i + 1))
done | sort -n | uniq | wc -l | tr -d ' ' | grep -q '^12$'

STATUS=$(curl_json POST "$BASE_URL/query" "{\"sql\":\"SELECT * FROM users;\"}" "$TMP_DIR/final_count.json")
assert_code 200 "$STATUS"
assert_jq "$TMP_DIR/final_count.json" '.success == true and .row_count == 13'

kill "$SERVER_PID" >/dev/null 2>&1 || true
wait "$SERVER_PID" >/dev/null 2>&1 || true
SERVER_PID=""

QUEUE_DB="$TMP_DIR/queue_db"
mkdir -p "$QUEUE_DB"
python3 - "$QUEUE_DB" <<'PY'
import pathlib
import sys

db_dir = pathlib.Path(sys.argv[1])
(db_dir / "users.schema").write_text("id\nname\nage\n", encoding="utf-8")
with (db_dir / "users.data").open("w", encoding="utf-8") as handle:
    for i in range(1, 50001):
        handle.write(f"{i}|user_{i}|{20 + (i % 50)}\n")
PY

PORT=$(get_free_port)
BASE_URL="http://127.0.0.1:$PORT"
QUEUE_LOG="$TMP_DIR/queue_server.log"
start_server "$QUEUE_DB" "$PORT" 1 1 "$QUEUE_LOG"

python3 - "$PORT" <<'PY'
import json
import socket
import sys
import threading
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
busy_results = {}
overflow_results = []


def send_request(method, path, body=None, timeout=20.0):
    body_bytes = body.encode("utf-8") if body is not None else b""
    request_lines = [
        f"{method} {path} HTTP/1.1",
        f"Host: {HOST}:{PORT}",
        "Connection: close",
    ]

    if body is not None:
        request_lines.append("Content-Type: application/json")
        request_lines.append(f"Content-Length: {len(body_bytes)}")

    request = ("\r\n".join(request_lines) + "\r\n\r\n").encode("ascii") + body_bytes
    response = bytearray()

    with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        while True:
            try:
                chunk = sock.recv(4096)
            except ConnectionResetError:
                break
            if not chunk:
                break
            response.extend(chunk)

    if not response:
        raise RuntimeError("empty HTTP response")

    header_bytes, separator, body_bytes = bytes(response).partition(b"\r\n\r\n")
    if not separator:
        raise RuntimeError("malformed HTTP response")

    status_line = header_bytes.split(b"\r\n", 1)[0].decode("ascii", "replace")
    parts = status_line.split()
    if len(parts) < 2 or not parts[1].isdigit():
        raise RuntimeError(f"invalid status line: {status_line}")

    return int(parts[1]), body_bytes.decode("utf-8", "replace")


def expect_json(body):
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON body: {body}") from exc


def run_busy(name, method, path, body=None):
    try:
        busy_results[name] = send_request(method, path, body)
    except Exception as exc:  # noqa: BLE001
        busy_results[name] = (0, "", str(exc))


busy_thread = threading.Thread(
    target=run_busy,
    args=("query", "POST", "/query", '{"sql":"SELECT * FROM users WHERE name = \'user_49999\';"}'),
)
busy_thread.start()

time.sleep(1.0)

def run_overflow():
    try:
        overflow_results.append(send_request("GET", "/health"))
    except Exception as exc:  # noqa: BLE001
        overflow_results.append((0, "", str(exc)))


overflow_threads = []
for _ in range(8):
    thread = threading.Thread(target=run_overflow)
    thread.start()
    overflow_threads.append(thread)

for thread in overflow_threads:
    thread.join()

busy_thread.join()
queue_rejected = 0

for result in overflow_results:
    status, body, *error = result
    if status == 503:
        payload = expect_json(body)
        if payload.get("error_code") != "QUEUE_FULL":
            raise RuntimeError(f"expected QUEUE_FULL body, got {payload}")
        queue_rejected += 1
    elif status == 200:
        payload = expect_json(body)
        if payload.get("success") is not True or payload.get("service") != "mini_db_server":
            raise RuntimeError(f"unexpected health payload: {payload}")
    elif status == 0:
        pass
    else:
        raise RuntimeError(f"unexpected overflow status: {status} error={error}")

query_status, query_body, *query_error = busy_results["query"]
if query_status != 200:
    raise RuntimeError(f"busy SELECT status was {query_status} error={query_error}")
query_payload = expect_json(query_body)
if query_payload.get("type") != "select" or query_payload.get("used_index") is not False:
    raise RuntimeError(f"busy SELECT payload mismatch: {query_payload}")

if queue_rejected <= 0:
    raise RuntimeError("expected at least one QUEUE_FULL response")
PY

echo "api_server: OK"
