#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
RESULT_PATH="${RESULT_PATH:-$ROOT_DIR/build/bench_api_server_results.tsv}"
RUNS_PATH="${RUNS_PATH:-$ROOT_DIR/build/bench_api_server_runs.tsv}"
REQUESTS_PER_RUN="${REQUESTS_PER_RUN:-1000}"
RUNS_PER_CASE="${RUNS_PER_CASE:-3}"
CONCURRENCY="${CONCURRENCY:-32}"
SEED_ROWS="${SEED_ROWS:-2000}"
WORKERS_LIST="${WORKERS_LIST:-2 4 8}"
QUEUE_SIZES="${QUEUE_SIZES:-32 64 128}"
WORKLOADS="${WORKLOADS:-select-only insert-only mixed}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mini-db-server-bench.XXXXXX")
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
    workers=$3
    queue_size=$4
    log_file=$5

    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
        SERVER_PID=""
    fi

    "$BIN_PATH" -d "$db_dir" -p "$port" -t "$workers" -q "$queue_size" >"$log_file" 2>&1 &
    SERVER_PID=$!
    wait_for_server "$port"
}

seed_db() {
    db_dir=$1
    rows=$2
    python3 - "$db_dir" "$rows" <<'PY'
import pathlib
import sys

db_dir = pathlib.Path(sys.argv[1])
rows = int(sys.argv[2])
db_dir.mkdir(parents=True, exist_ok=True)
(db_dir / "users.schema").write_text("id\nname\nage\n", encoding="utf-8")
with (db_dir / "users.data").open("w", encoding="utf-8") as handle:
    for i in range(1, rows + 1):
        handle.write(f"{i}|user_{i}|{20 + (i % 50)}\n")
PY
}

empty_db() {
    db_dir=$1
    mkdir -p "$db_dir"
    cat > "$db_dir/users.schema" <<'EOF'
id
name
age
EOF
    : > "$db_dir/users.data"
}

emit_request() {
    workload=$1
    index=$2
    total_seed_rows=$3

    case "$workload" in
        select-only)
            probe_id=$(( (index % total_seed_rows) + 1 ))
            printf '{"sql":"SELECT * FROM users WHERE id = %s;"}' "$probe_id"
            ;;
        insert-only)
            printf '{"sql":"INSERT INTO users VALUES ('\''bench_user_%s'\'', %s);"}' "$index" $((20 + (index % 50)))
            ;;
        mixed)
            if [ $((index % 2)) -eq 0 ]; then
                probe_id=$(( (index % total_seed_rows) + 1 ))
                printf '{"sql":"SELECT * FROM users WHERE id = %s;"}' "$probe_id"
            else
                printf '{"sql":"INSERT INTO users VALUES ('\''bench_mix_%s'\'', %s);"}' "$index" $((20 + (index % 50)))
            fi
            ;;
        *)
            return 1
            ;;
    esac
}

run_batch() {
    base_url=$1
    workload=$2
    output_file=$3
    total_seed_rows=$4
    pids=""
    in_flight=0

    : > "$output_file"
    i=1
    while [ "$i" -le "$REQUESTS_PER_RUN" ]; do
        request_index=$i
        (
            payload=$(emit_request "$workload" "$request_index" "$total_seed_rows")
            if result=$(
                curl --max-time 20 -sS -o /dev/null \
                    -w "%{http_code}\t%{time_total}" \
                    -X POST "$base_url/query" \
                    -H "Content-Type: application/json" \
                    --data "$payload" \
                    2>>"$TMP_DIR/curl_errors.log"
            ); then
                printf '%s\t%s\n' "$request_index" "$result" >> "$output_file"
            else
                printf '%s\t000\t20.000000\n' "$request_index" >> "$output_file"
            fi
        ) &
        pids="$pids $!"
        in_flight=$((in_flight + 1))

        if [ "$in_flight" -ge "$CONCURRENCY" ]; then
            for pid in $pids; do
                wait "$pid"
            done
            pids=""
            in_flight=0
        fi

        i=$((i + 1))
    done

    for pid in $pids; do
        wait "$pid"
    done
}

mkdir -p "$(dirname "$RESULT_PATH")"
mkdir -p "$(dirname "$RUNS_PATH")"

case "$CONCURRENCY" in
    ''|*[!0-9]*)
        echo "CONCURRENCY must be a positive integer" >&2
        exit 1
        ;;
esac

if [ "$CONCURRENCY" -le 0 ]; then
    echo "CONCURRENCY must be a positive integer" >&2
    exit 1
fi

: > "$TMP_DIR/curl_errors.log"

printf 'workers\tqueue_size\tworkload\tconcurrency\trun\ttotal_requests\tsuccess_requests\trejected_requests\tsuccessful_insert_requests\ttotal_ms\tavg_ms\tp95_ms\tsum_latency_ms\tparallelism_estimate\tthroughput_rps\texpected_rows\tactual_rows\tunique_id_count\trows_ok\tids_unique_ok\tid_sequence_ok\n' > "$RUNS_PATH"

for workers in $WORKERS_LIST; do
    for queue_size in $QUEUE_SIZES; do
        for workload in $WORKLOADS; do
            run_index=1
            while [ "$run_index" -le "$RUNS_PER_CASE" ]; do
                DB_DIR="$TMP_DIR/${workers}_${queue_size}_${workload}_run${run_index}_db"
                LOG_FILE="$TMP_DIR/${workers}_${queue_size}_${workload}_run${run_index}.log"
                SAMPLE_FILE="$TMP_DIR/${workers}_${queue_size}_${workload}_run${run_index}.tsv"
                PORT=$(get_free_port)
                BASE_URL="http://127.0.0.1:$PORT"

                case "$workload" in
                    select-only)
                        seed_db "$DB_DIR" "$SEED_ROWS"
                        initial_rows="$SEED_ROWS"
                        active_seed_rows="$SEED_ROWS"
                        ;;
                    insert-only)
                        empty_db "$DB_DIR"
                        initial_rows="0"
                        active_seed_rows="1"
                        ;;
                    mixed)
                        seed_db "$DB_DIR" "$SEED_ROWS"
                        initial_rows="$SEED_ROWS"
                        active_seed_rows="$SEED_ROWS"
                        ;;
                    *)
                        echo "unsupported workload: $workload" >&2
                        exit 1
                        ;;
                esac

                START_MS=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)

                printf '[run] workers=%s queue=%s workload=%s run=%s/%s requests=%s concurrency=%s\n' \
                    "$workers" "$queue_size" "$workload" "$run_index" "$RUNS_PER_CASE" \
                    "$REQUESTS_PER_RUN" "$CONCURRENCY"

                if ! start_server "$DB_DIR" "$PORT" "$workers" "$queue_size" "$LOG_FILE"; then
                    cat "$LOG_FILE" >&2
                    exit 1
                fi

                run_batch "$BASE_URL" "$workload" "$SAMPLE_FILE" "$active_seed_rows"

                END_MS=$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)

                kill "$SERVER_PID" >/dev/null 2>&1 || true
                wait "$SERVER_PID" >/dev/null 2>&1 || true
                SERVER_PID=""

                python3 - "$workers" "$queue_size" "$workload" "$CONCURRENCY" "$run_index" "$REQUESTS_PER_RUN" "$START_MS" "$END_MS" "$SAMPLE_FILE" "$DB_DIR/users.data" "$initial_rows" >> "$RUNS_PATH" <<'PY'
import math
import statistics
import sys

workers = sys.argv[1]
queue_size = sys.argv[2]
workload = sys.argv[3]
concurrency = sys.argv[4]
run_index = sys.argv[5]
total_requests = int(sys.argv[6])
start_ms = int(sys.argv[7])
end_ms = int(sys.argv[8])
sample_file = sys.argv[9]
data_file = sys.argv[10]
initial_rows = int(sys.argv[11])

entries = []
with open(sample_file, "r", encoding="utf-8") as handle:
    for line in handle:
        line = line.strip()
        if not line:
            continue
        index_text, status_text, time_text = line.split("\t", 2)
        entries.append((int(index_text), int(status_text), float(time_text) * 1000.0))

entries.sort(key=lambda item: item[0])
statuses = [status for _, status, _ in entries]
latencies_ms = [latency for _, _, latency in entries]

success_requests = sum(1 for status in statuses if status == 200)
rejected_requests = sum(1 for status in statuses if status == 503)

if workload == "insert-only":
    successful_insert_requests = sum(1 for _, status, _ in entries if status == 200)
elif workload == "mixed":
    successful_insert_requests = sum(
        1 for index, status, _ in entries
        if index % 2 == 1 and status == 200
    )
else:
    successful_insert_requests = 0

avg_ms = statistics.mean(latencies_ms) if latencies_ms else 0.0
if latencies_ms:
    ordered = sorted(latencies_ms)
    p95_index = max(0, math.ceil(len(ordered) * 0.95) - 1)
    p95_ms = ordered[p95_index]
else:
    p95_ms = 0.0

sum_latency_ms = sum(latencies_ms)
total_ms = float(end_ms - start_ms)
parallelism_estimate = (sum_latency_ms / total_ms) if total_ms > 0.0 else 0.0
throughput_rps = ((success_requests * 1000.0) / total_ms) if total_ms > 0.0 else 0.0

actual_rows = 0
parsed_ids = []
with open(data_file, "r", encoding="utf-8") as handle:
    for raw_line in handle:
        line = raw_line.rstrip("\n")
        if line == "":
            continue
        actual_rows += 1
        first_field = line.split("|", 1)[0]
        try:
            parsed_ids.append(int(first_field))
        except ValueError:
            pass

unique_id_count = len(set(parsed_ids))
expected_rows = initial_rows + successful_insert_requests
rows_ok = actual_rows == expected_rows
ids_unique_ok = len(parsed_ids) == actual_rows and unique_id_count == actual_rows

if actual_rows == 0:
    id_sequence_ok = True
elif ids_unique_ok:
    id_sequence_ok = min(parsed_ids) == 1 and max(parsed_ids) == actual_rows
else:
    id_sequence_ok = False

print(
    f"{workers}\t{queue_size}\t{workload}\t{concurrency}\t{run_index}\t{total_requests}\t"
    f"{success_requests}\t{rejected_requests}\t{successful_insert_requests}\t"
    f"{total_ms:.2f}\t{avg_ms:.2f}\t{p95_ms:.2f}\t{sum_latency_ms:.2f}\t"
    f"{parallelism_estimate:.2f}\t{throughput_rps:.2f}\t{expected_rows}\t"
    f"{actual_rows}\t{unique_id_count}\t"
    f"{str(rows_ok).lower()}\t{str(ids_unique_ok).lower()}\t{str(id_sequence_ok).lower()}"
)
PY

                run_index=$((run_index + 1))
            done
        done
    done
done

python3 - "$RUNS_PATH" > "$RESULT_PATH" <<'PY'
import csv
import sys
from collections import defaultdict

runs_path = sys.argv[1]
groups = defaultdict(list)

with open(runs_path, "r", encoding="utf-8") as handle:
    reader = csv.DictReader(handle, delimiter="\t")
    for row in reader:
        key = (
            int(row["workers"]),
            int(row["queue_size"]),
            row["workload"],
            int(row["concurrency"]),
        )
        groups[key].append(row)

header = [
    "workers",
    "queue_size",
    "workload",
    "concurrency",
    "runs",
    "avg_total_requests",
    "avg_success_requests",
    "avg_rejected_requests",
    "avg_successful_insert_requests",
    "avg_total_ms",
    "avg_avg_ms",
    "avg_p95_ms",
    "avg_parallelism_estimate",
    "avg_throughput_rps",
    "avg_expected_rows",
    "avg_actual_rows",
    "all_rows_ok",
    "all_ids_unique_ok",
    "all_id_sequence_ok",
]
print("\t".join(header))

def avg(rows, field):
    return sum(float(row[field]) for row in rows) / float(len(rows))

def all_true(rows, field):
    return all(row[field] == "true" for row in rows)

for workers, queue_size, workload, concurrency in sorted(groups.keys()):
    rows = groups[(workers, queue_size, workload, concurrency)]
    print(
        f"{workers}\t{queue_size}\t{workload}\t{concurrency}\t{len(rows)}\t"
        f"{avg(rows, 'total_requests'):.2f}\t"
        f"{avg(rows, 'success_requests'):.2f}\t"
        f"{avg(rows, 'rejected_requests'):.2f}\t"
        f"{avg(rows, 'successful_insert_requests'):.2f}\t"
        f"{avg(rows, 'total_ms'):.2f}\t"
        f"{avg(rows, 'avg_ms'):.2f}\t"
        f"{avg(rows, 'p95_ms'):.2f}\t"
        f"{avg(rows, 'parallelism_estimate'):.2f}\t"
        f"{avg(rows, 'throughput_rps'):.2f}\t"
        f"{avg(rows, 'expected_rows'):.2f}\t"
        f"{avg(rows, 'actual_rows'):.2f}\t"
        f"{str(all_true(rows, 'rows_ok')).lower()}\t"
        f"{str(all_true(rows, 'ids_unique_ok')).lower()}\t"
        f"{str(all_true(rows, 'id_sequence_ok')).lower()}"
    )
PY

printf '[summary] %s\n' "$RESULT_PATH"
cat "$RESULT_PATH"
printf '\n[per-run] %s\n' "$RUNS_PATH"

if [ -s "$TMP_DIR/curl_errors.log" ]; then
    printf '[curl-errors] %s\n' "$TMP_DIR/curl_errors.log"
fi
