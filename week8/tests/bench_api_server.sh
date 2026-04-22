#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BIN_PATH="${MINI_DB_SERVER_BIN:-$ROOT_DIR/mini_db_server}"
OUT_FILE="${OUT_FILE:-$ROOT_DIR/build/api_benchmark.csv}"
RAW_OUT_FILE="${RAW_OUT_FILE:-$ROOT_DIR/build/api_benchmark_runs.csv}"
SUMMARY_FILE="${SUMMARY_FILE:-$ROOT_DIR/build/api_benchmark_summary.txt}"
REPORT_FILE="${REPORT_FILE:-$ROOT_DIR/build/api_benchmark_report.html}"
TOTAL_REQUESTS="${TOTAL_REQUESTS:-2000}"
REPEAT_RUNS="${REPEAT_RUNS:-3}"
CONCURRENCY="${CONCURRENCY:-64}"
WORKERS_LIST="${WORKERS_LIST:-1 2 4 8}"
QUEUE_LIST="${QUEUE_LIST:-32 64 128}"
WORKLOADS="${WORKLOADS:-select-only insert-only mixed}"
BASE_PORT="${BASE_PORT:-19080}"

mkdir -p "$(dirname "$OUT_FILE")"

python3 - "$BIN_PATH" "$OUT_FILE" "$RAW_OUT_FILE" "$SUMMARY_FILE" "$REPORT_FILE" "$TOTAL_REQUESTS" "$REPEAT_RUNS" "$CONCURRENCY" "$WORKERS_LIST" "$QUEUE_LIST" "$WORKLOADS" "$BASE_PORT" <<'PY'
import csv
import html
import http.client
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

bin_path, out_file, raw_out_file, summary_file, report_file = sys.argv[1:6]
total_requests = int(sys.argv[6])
repeat_runs = int(sys.argv[7])
concurrency = int(sys.argv[8])
workers_list = [int(x) for x in sys.argv[9].split()]
queue_list = [int(x) for x in sys.argv[10].split()]
workloads = sys.argv[11].split()
base_port = int(sys.argv[12])

def write_schema(db_dir):
    os.makedirs(db_dir, exist_ok=True)
    with open(os.path.join(db_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write("id\nname\nage\n")

def request(port, sql):
    start = time.perf_counter()
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    body = json.dumps({"sql": sql})
    try:
        conn.request("POST", "/query", body=body, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        status = resp.status
    except Exception:
        status = 0
    finally:
        conn.close()
    return status, (time.perf_counter() - start) * 1000.0

def wait_ready(port):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/health")
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status == 200:
                return
        except Exception:
            time.sleep(0.05)
    raise RuntimeError("server did not become ready")

def sql_for(workload, index):
    if workload == "select-only":
        return "SELECT * FROM users WHERE id = 1;"
    if workload == "insert-only":
        return "INSERT INTO users VALUES ('bench%d', %d);" % (index, 20 + (index % 50))
    if index % 2 == 0:
        return "SELECT * FROM users WHERE id = 1;"
    return "INSERT INTO users VALUES ('mixed%d', %d);" % (index, 20 + (index % 50))

def percentile(values, percent):
    if not values:
        return 0.0
    index = max(0, min(len(values) - 1, int(len(values) * percent / 100.0) - 1))
    return sorted(values)[index]

def run_case(run_index, workers, queue_size, workload, port):
    tmp = tempfile.mkdtemp(prefix="mini-db-bench.")
    db_dir = os.path.join(tmp, "db")
    write_schema(db_dir)
    proc = subprocess.Popen(
        [bin_path, "-d", db_dir, "-p", str(port), "-t", str(workers), "-q", str(queue_size)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_ready(port)
        seed_status, _ = request(port, "INSERT INTO users VALUES ('seed', 1);")
        if seed_status != 200:
            raise RuntimeError("seed insert failed")

        started = time.perf_counter()
        with ThreadPoolExecutor(max_workers=concurrency) as executor:
            results = list(executor.map(lambda i: request(port, sql_for(workload, i)), range(total_requests)))
        total_ms = (time.perf_counter() - started) * 1000.0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
        shutil.rmtree(tmp, ignore_errors=True)

    statuses = [status for status, _ in results]
    latencies = [latency for _, latency in results]
    success = sum(1 for status in statuses if status == 200)
    rejected = sum(1 for status in statuses if status == 503)
    failures = total_requests - success
    avg_ms = statistics.mean(latencies) if latencies else 0.0
    p95_ms = percentile(latencies, 95)

    return {
        "run": run_index,
        "workers": workers,
        "queue_size": queue_size,
        "workload": workload,
        "total_requests": total_requests,
        "success_requests": success,
        "rejected_requests": rejected,
        "failure_requests": failures,
        "total_ms": round(total_ms, 2),
        "avg_ms": round(avg_ms, 2),
        "p95_ms": round(p95_ms, 2),
    }

def group_key(row):
    return row["workers"], row["queue_size"], row["workload"]

def status_for(row):
    if row["failure_requests_total"] == 0 and row["rejected_requests_total"] == 0:
        return "OK"
    if row["failure_requests_total"] == row["rejected_requests_total"]:
        return "WARN_503"
    return "BAD"

def aggregate_rows(raw_rows):
    groups = {}
    aggregate = []

    for row in raw_rows:
        groups.setdefault(group_key(row), []).append(row)

    for key in sorted(groups):
        rows = groups[key]
        workers, queue_size, workload = key
        total_requests_all = sum(row["total_requests"] for row in rows)
        success_total = sum(row["success_requests"] for row in rows)
        rejected_total = sum(row["rejected_requests"] for row in rows)
        failure_total = sum(row["failure_requests"] for row in rows)
        summary = {
            "workers": workers,
            "queue_size": queue_size,
            "workload": workload,
            "runs": len(rows),
            "requests_per_run": total_requests,
            "total_requests_all": total_requests_all,
            "success_requests_total": success_total,
            "rejected_requests_total": rejected_total,
            "failure_requests_total": failure_total,
            "success_rate_pct": round((success_total / total_requests_all) * 100.0, 2) if total_requests_all else 0.0,
            "rejected_rate_pct": round((rejected_total / total_requests_all) * 100.0, 2) if total_requests_all else 0.0,
            "total_ms_avg": round(statistics.mean(row["total_ms"] for row in rows), 2),
            "avg_ms_mean": round(statistics.mean(row["avg_ms"] for row in rows), 2),
            "p95_ms_mean": round(statistics.mean(row["p95_ms"] for row in rows), 2),
            "p95_ms_max": round(max(row["p95_ms"] for row in rows), 2),
        }
        summary["status"] = status_for(summary)
        aggregate.append(summary)

    return aggregate

def choose_best(aggregate):
    mixed_ok = [
        row for row in aggregate
        if row["workload"] == "mixed" and row["rejected_requests_total"] == 0 and row["failure_requests_total"] == 0
    ]
    if mixed_ok:
        return min(mixed_ok, key=lambda row: (row["p95_ms_mean"], row["avg_ms_mean"], row["workers"], row["queue_size"]))

    mixed = [row for row in aggregate if row["workload"] == "mixed"]
    if mixed:
        return min(mixed, key=lambda row: (
            row["rejected_rate_pct"],
            row["failure_requests_total"],
            row["p95_ms_mean"],
            row["avg_ms_mean"],
        ))
    return None

def write_csv(path, rows, fields):
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

def write_summary(path, aggregate, best):
    mixed_rows = [row for row in aggregate if row["workload"] == "mixed"]
    mixed_rows.sort(key=lambda row: (row["status"] != "OK", row["p95_ms_mean"], row["avg_ms_mean"]))

    with open(path, "w", encoding="utf-8") as f:
        f.write("Mini DB API Benchmark Summary\n")
        f.write("requests_per_run=%d repeat_runs=%d concurrency=%d\n" % (total_requests, repeat_runs, concurrency))
        f.write("status_rule=OK means no 503 and no failures across all repeated runs\n\n")
        if best:
            f.write(
                "BEST_MIXED workers=%s queue_size=%s avg_ms_mean=%.2f p95_ms_mean=%.2f p95_ms_max=%.2f status=%s\n\n"
                % (
                    best["workers"],
                    best["queue_size"],
                    best["avg_ms_mean"],
                    best["p95_ms_mean"],
                    best["p95_ms_max"],
                    best["status"],
                )
            )
        f.write("Mixed workload ranking\n")
        f.write("workers queue success_rate rejected_rate avg_ms p95_mean p95_max status\n")
        for row in mixed_rows:
            f.write(
                "%7s %5s %11.2f %13.2f %6.2f %8.2f %7.2f %s\n"
                % (
                    row["workers"],
                    row["queue_size"],
                    row["success_rate_pct"],
                    row["rejected_rate_pct"],
                    row["avg_ms_mean"],
                    row["p95_ms_mean"],
                    row["p95_ms_max"],
                    row["status"],
                )
            )
        f.write("\nFiles\n")
        f.write("summary_csv=%s\n" % out_file)
        f.write("raw_runs_csv=%s\n" % raw_out_file)
        f.write("html_report=%s\n" % report_file)

def write_report(path, aggregate, best):
    rows = sorted(aggregate, key=lambda row: (row["workload"] != "mixed", row["status"] != "OK", row["p95_ms_mean"]))
    best_key = group_key(best) if best else None
    html_rows = []

    for row in rows:
        cls = "best" if best_key and group_key(row) == best_key else row["status"].lower()
        html_rows.append(
            "<tr class='%s'><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%.2f%%</td><td>%.2f%%</td><td>%.2f</td><td>%.2f</td><td>%.2f</td><td>%s</td></tr>"
            % (
                cls,
                html.escape(str(row["workers"])),
                html.escape(str(row["queue_size"])),
                html.escape(row["workload"]),
                html.escape(str(row["runs"])),
                row["success_rate_pct"],
                row["rejected_rate_pct"],
                row["avg_ms_mean"],
                row["p95_ms_mean"],
                row["p95_ms_max"],
                html.escape(row["status"]),
            )
        )

    best_text = "No mixed workload result"
    if best:
        best_text = "workers=%s, queue=%s, avg=%.2fms, p95=%.2fms, status=%s" % (
            best["workers"],
            best["queue_size"],
            best["avg_ms_mean"],
            best["p95_ms_mean"],
            best["status"],
        )

    document = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Mini DB API Benchmark Report</title>
<style>
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; margin: 24px; color: #111; }
h1 { margin-bottom: 4px; }
.meta { color: #555; margin-bottom: 20px; }
.best-box { border: 1px solid #111; padding: 12px; margin-bottom: 16px; background: #f7f7f7; }
table { border-collapse: collapse; width: 100%%; font-size: 14px; }
th, td { border: 1px solid #ddd; padding: 8px; text-align: right; }
th:nth-child(3), td:nth-child(3), th:last-child, td:last-child { text-align: left; }
th { background: #efefef; }
tr.best { background: #dff5e1; font-weight: 700; }
tr.warn_503 { background: #fff5d6; }
tr.bad { background: #ffe0e0; }
</style>
</head>
<body>
<h1>Mini DB API Benchmark Report</h1>
<div class="meta">requests_per_run=%d, repeat_runs=%d, concurrency=%d</div>
<div class="best-box"><strong>Best mixed:</strong> %s</div>
<table>
<thead>
<tr><th>workers</th><th>queue</th><th>workload</th><th>runs</th><th>success</th><th>rejected</th><th>avg ms</th><th>p95 mean</th><th>p95 max</th><th>status</th></tr>
</thead>
<tbody>
%s
</tbody>
</table>
</body>
</html>
""" % (total_requests, repeat_runs, concurrency, html.escape(best_text), "\n".join(html_rows))

    with open(path, "w", encoding="utf-8") as f:
        f.write(document)

raw_rows = []
case_index = 0
for workers in workers_list:
    for queue_size in queue_list:
        for workload in workloads:
            for run_index in range(1, repeat_runs + 1):
                port = base_port + case_index
                case_index += 1
                row = run_case(run_index, workers, queue_size, workload, port)
                raw_rows.append(row)
                print(
                    "run=%s/%s workers=%s queue=%s workload=%s success=%s rejected=%s total_ms=%s avg_ms=%s p95_ms=%s"
                    % (
                        row["run"],
                        repeat_runs,
                        row["workers"],
                        row["queue_size"],
                        row["workload"],
                        row["success_requests"],
                        row["rejected_requests"],
                        row["total_ms"],
                        row["avg_ms"],
                        row["p95_ms"],
                    )
                )

raw_fields = [
    "run",
    "workers",
    "queue_size",
    "workload",
    "total_requests",
    "success_requests",
    "rejected_requests",
    "failure_requests",
    "total_ms",
    "avg_ms",
    "p95_ms",
]
summary_fields = [
    "workers",
    "queue_size",
    "workload",
    "runs",
    "requests_per_run",
    "total_requests_all",
    "success_requests_total",
    "rejected_requests_total",
    "failure_requests_total",
    "success_rate_pct",
    "rejected_rate_pct",
    "total_ms_avg",
    "avg_ms_mean",
    "p95_ms_mean",
    "p95_ms_max",
    "status",
]

summary_rows = aggregate_rows(raw_rows)
best = choose_best(summary_rows)

write_csv(raw_out_file, raw_rows, raw_fields)
write_csv(out_file, summary_rows, summary_fields)
write_summary(summary_file, summary_rows, best)
write_report(report_file, summary_rows, best)

if best:
    print(
        "recommended_default workers=%s queue_size=%s workload=mixed avg_ms_mean=%s p95_ms_mean=%s p95_ms_max=%s status=%s"
        % (
            best["workers"],
            best["queue_size"],
            best["avg_ms_mean"],
            best["p95_ms_mean"],
            best["p95_ms_max"],
            best["status"],
        )
    )
print("wrote %s" % out_file)
print("wrote %s" % raw_out_file)
print("wrote %s" % summary_file)
print("wrote %s" % report_file)
PY
