#!/usr/bin/env python3
"""Timed memtier matrix. Does not import or edit mutr source.

One row is appended to bench/results/runs.csv after every run.
A row already in that file is skipped, so the script can be restarted.
"""

import csv
import json
import os
import resource
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def results_dir():
    """Guest virtiofs export of the repo is not writable, so results land on
    the guest disk unless MUTR_BENCH_OUT is set or the repo path accepts a
    create."""
    override = os.environ.get("MUTR_BENCH_OUT")
    if override:
        return override
    candidate = os.path.join(ROOT, "bench", "results")
    try:
        os.makedirs(candidate, exist_ok=True)
        probe = os.path.join(candidate, ".write_probe")
        with open(probe, "w", encoding="utf-8") as fh:
            fh.write("ok")
        os.remove(probe)
        return candidate
    except OSError:
        return "/var/tmp/mutr-bench-results"


RES = results_dir()
RAW = os.path.join(RES, "raw")
CSV_PATH = os.path.join(RES, "runs.csv")
MUTR = "/tmp/mutr-bench/mutr"
REDIS = "/tmp/redis-6.2.20/src/redis-server"
MEMTIER = "/tmp/memtier_benchmark/memtier_benchmark"
PORT = "17200"
SERVER_CORES = "0-3"
CLIENT_CORES = "4-7"
CLIENT_CORE_LIST = (4, 5, 6, 7)

COLUMNS = [
    "server", "threads", "shards", "redis_io_threads",
    "value_size", "mix", "connections", "pipeline", "rep",
    "ops_per_sec", "p50_ms", "p99_ms", "p99_9_ms",
    "gets_per_sec", "hits_per_sec", "misses_per_sec",
    "set_count_preload", "connection_errors",
    "server_cpu_avg", "client_cpu_avg", "client_peak_pct",
    "client_core_idle", "client_bound", "valid", "invalid_reason",
    "raw_stdout", "raw_json",
]


def servers():
    return [
        {"server": "mutr", "threads": 1, "shards": 64, "redis_io_threads": 0},
        {"server": "redis", "threads": 1, "shards": "", "redis_io_threads": 0},
        {"server": "mutr", "threads": 2, "shards": 64, "redis_io_threads": 0},
        {"server": "redis", "threads": 4, "shards": "", "redis_io_threads": 4},
        {"server": "mutr", "threads": 4, "shards": 64, "redis_io_threads": 0},
        {"server": "mutr", "threads": 4, "shards": 1, "redis_io_threads": 0},
    ]


def workloads():
    rows = []
    for size in (64, 1024):
        for mix in ("set", "get", "mixed"):
            for conns in (8, 32, 128, 512):
                for pipe in (1, 16):
                    rows.append({
                        "value_size": size,
                        "mix": mix,
                        "connections": conns,
                        "pipeline": pipe,
                    })
    return rows


def key_of(spec, work, rep):
    return (
        spec["server"], str(spec["threads"]), str(spec["shards"]),
        str(spec["redis_io_threads"]), str(work["value_size"]), work["mix"],
        str(work["connections"]), str(work["pipeline"]), str(rep),
    )


def load_done():
    done = set()
    if not os.path.exists(CSV_PATH):
        return done
    with open(CSV_PATH, newline="") as fh:
        for row in csv.DictReader(fh):
            done.add((
                row["server"], row["threads"], row["shards"], row["redis_io_threads"],
                row["value_size"], row["mix"], row["connections"], row["pipeline"], row["rep"],
            ))
    return done


def ensure_csv():
    os.makedirs(RAW, exist_ok=True)
    if not os.path.exists(CSV_PATH):
        with open(CSV_PATH, "w", newline="") as fh:
            csv.DictWriter(fh, fieldnames=COLUMNS).writeheader()


def append_row(row):
    with open(CSV_PATH, "a", newline="") as fh:
        csv.DictWriter(fh, fieldnames=COLUMNS).writerow(row)
        fh.flush()
        os.fsync(fh.fileno())


def log(msg):
    line = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) + " " + msg
    print(line, flush=True)
    with open(os.path.join(RES, "progress.log"), "a") as fh:
        fh.write(line + "\n")


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def start_server(spec, err_path):
    if spec["server"] == "mutr":
        cmd = [
            "taskset", "-c", SERVER_CORES, MUTR,
            "--port", PORT, "--threads", str(spec["threads"]),
            "--shards", str(spec["shards"]),
        ]
        needle = "listening " + PORT
    else:
        cmd = [
            "taskset", "-c", SERVER_CORES, REDIS,
            "--port", PORT, "--bind", "127.0.0.1",
            "--save", "", "--appendonly", "no", "--protected-mode", "no",
            "--daemonize", "no",
        ]
        if spec["redis_io_threads"]:
            cmd += ["--io-threads", "4", "--io-threads-do-reads", "yes"]
        needle = "Ready to accept"
    # Redis logs readiness on stdout; mutr logs it on stderr.
    err = open(err_path, "wb", buffering=0)
    proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    deadline = time.time() + 15
    while time.time() < deadline:
        if proc.poll() is not None:
            err.flush()
            raise RuntimeError("server exited early: " + open(err_path).read()[-500:])
        if needle in open(err_path).read():
            return proc
        time.sleep(0.05)
    stop(proc)
    raise RuntimeError("server not ready: " + open(err_path).read()[-500:])


def memtier_cmd(work, seconds, allkeys=False):
    cmd = [
        "taskset", "-c", CLIENT_CORES, MEMTIER,
        "-s", "127.0.0.1", "-p", PORT, "--protocol=redis",
        "--key-prefix=memtier-", "--key-minimum=1", "--key-maximum=1000000",
        "--distinct-client-seed",
        "--data-size", str(work["value_size"]),
    ]
    if allkeys:
        # One pass over keys 1..1000000. 4 threads * 8 clients * allkeys.
        cmd += ["-t", "4", "-c", "8", "--pipeline", "32", "--ratio=1:0",
                "--key-pattern=P:P", "-n", "allkeys"]
    else:
        ratio = {"set": "1:0", "get": "0:1", "mixed": "1:1"}[work["mix"]]
        # This memtier rejects --requests together with --test-time. With only
        # --test-time set, it does not apply the default of 10000 requests.
        cmd += [
            "-t", "4", "-c", str(work["connections"] // 4),
            "--pipeline", str(work["pipeline"]),
            "--ratio", ratio, "--key-pattern=R:R",
            "--test-time", str(seconds),
        ]
    return cmd


def run_memtier(cmd, stdout_path, json_path):
    full = cmd + ["--json-out-file", json_path]
    with open(stdout_path, "w") as out:
        proc = subprocess.run(full, stdout=out, stderr=subprocess.STDOUT)
    return proc.returncode


def preload(work, stdout_path, json_path):
    cmd = memtier_cmd(work, 0, allkeys=True)
    rc = run_memtier(cmd, stdout_path, json_path)
    if rc != 0 or not os.path.exists(json_path):
        return None
    with open(json_path) as fh:
        data = json.load(fh, parse_float=str, parse_int=str)
    return data["ALL STATS"]["Sets"]["Count"]


def parse_cpu(pidstat_path, mpstat_path):
    server_avg = ""
    # pidstat Average line for the whole process contains the pid and %CPU near the end.
    if os.path.exists(pidstat_path):
        for line in open(pidstat_path):
            if not line.startswith("Average:"):
                continue
            parts = line.split()
            # Average: UID PID %usr %system %guest %wait %CPU CPU Command
            if len(parts) >= 8 and parts[2].isdigit():
                server_avg = parts[7]
    idle = {}
    if os.path.exists(mpstat_path):
        header = None
        for line in open(mpstat_path):
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "Average:" and len(parts) > 2 and parts[1] == "CPU":
                header = parts
                continue
            if parts[0] == "Average:" and header and len(parts) == len(header):
                cpu = parts[1]
                if cpu.isdigit() and int(cpu) in CLIENT_CORE_LIST:
                    idle[cpu] = parts[header.index("%idle")]
    return server_avg, idle


def client_bound(per_thread, idle):
    """A client core is saturated when mpstat shows it with under 5% idle,
    or when one memtier thread reports cores_used >= 0.95.

    memtier's peak_cpu_utilization_pct is an aggregate across threads. On a
    4-thread client it exceeds 100 while every client core is still mostly
    idle, so it is recorded and not used as the saturation test.
    """
    reasons = []
    for name, cores in per_thread:
        try:
            if float(cores) >= 0.95:
                reasons.append(name + "_cores_" + str(cores))
        except ValueError:
            pass
    for cpu, value in idle.items():
        try:
            if float(value) < 5.0:
                reasons.append(f"core{cpu}_idle_{value}")
        except ValueError:
            pass
    return ("1" if reasons else "0"), ";".join(reasons)


def measure(spec, work, rep, server_proc):
    tag = (
        f"{spec['server']}_t{spec['threads']}_s{spec['shards']}"
        f"_io{spec['redis_io_threads']}_{work['mix']}_{work['value_size']}"
        f"_c{work['connections']}_p{work['pipeline']}_r{rep}"
    )
    folder = os.path.join(RAW, tag)
    os.makedirs(folder, exist_ok=True)
    row = {c: "" for c in COLUMNS}
    row.update({
        "server": spec["server"],
        "threads": spec["threads"],
        "shards": spec["shards"],
        "redis_io_threads": spec["redis_io_threads"],
        "value_size": work["value_size"],
        "mix": work["mix"],
        "connections": work["connections"],
        "pipeline": work["pipeline"],
        "rep": rep,
        "valid": "0",
        "client_bound": "0",
    })
    reason = []
    if work["mix"] != "set":
        count = preload(work, os.path.join(folder, "preload.stdout"), os.path.join(folder, "preload.json"))
        row["set_count_preload"] = "" if count is None else count
        if count != "1000000":
            reason.append("preload_count_" + str(count))
    # Warm-up is the same command stream and is not recorded as a result.
    warm_rc = run_memtier(
        memtier_cmd(work, 10),
        os.path.join(folder, "warmup.stdout"),
        os.path.join(folder, "warmup.json"),
    )
    if warm_rc != 0:
        reason.append("warmup_exit_" + str(warm_rc))

    stdout_path = os.path.join(folder, "run.stdout")
    json_path = os.path.join(folder, "run.json")
    pidstat_path = os.path.join(folder, "pidstat.txt")
    mpstat_path = os.path.join(folder, "mpstat.txt")
    pidstat = subprocess.Popen(
        ["pidstat", "-u", "-p", str(server_proc.pid), "1", "30"],
        stdout=open(pidstat_path, "w"), stderr=subprocess.STDOUT,
    )
    mpstat = subprocess.Popen(
        ["mpstat", "-P", "ALL", "1", "30"],
        stdout=open(mpstat_path, "w"), stderr=subprocess.STDOUT,
    )
    rc = run_memtier(memtier_cmd(work, 30), stdout_path, json_path)
    pidstat.wait()
    mpstat.wait()
    row["raw_stdout"] = os.path.relpath(stdout_path, RES)
    row["raw_json"] = os.path.relpath(json_path, RES)
    if rc != 0 or not os.path.exists(json_path):
        reason.append("memtier_exit_" + str(rc))
        row["invalid_reason"] = ";".join(reason)
        append_row(row)
        return row

    with open(json_path) as fh:
        data = json.load(fh, parse_float=str, parse_int=str)
    runtime = data["ALL STATS"]["Runtime"]
    if runtime.get("Time unit") != "MILLISECONDS":
        reason.append("runtime_unit_" + str(runtime.get("Time unit")))
    else:
        try:
            if int(runtime["Total duration"]) < 29000:
                reason.append("short_duration_" + str(runtime["Total duration"]))
        except (ValueError, TypeError):
            reason.append("duration_unparsed")
    totals = data["ALL STATS"]["Totals"]
    gets = data["ALL STATS"]["Gets"]
    cpu = data["ALL STATS"]["CPU"]
    pct = totals["Percentile Latencies"]
    row["ops_per_sec"] = totals["Ops/sec"]
    row["p50_ms"] = pct.get("p50.00", "")
    row["p99_ms"] = pct.get("p99.00", "")
    row["p99_9_ms"] = pct.get("p99.90", "")
    row["gets_per_sec"] = gets["Ops/sec"]
    row["hits_per_sec"] = gets["Hits/sec"]
    row["misses_per_sec"] = gets["Misses/sec"]
    row["connection_errors"] = totals["Connection Errors"]
    row["client_cpu_avg"] = cpu.get("avg_cpu_utilization_pct", "")
    row["client_peak_pct"] = cpu.get("peak_cpu_utilization_pct", "")
    server_avg, idle = parse_cpu(pidstat_path, mpstat_path)
    row["server_cpu_avg"] = server_avg
    row["client_core_idle"] = ",".join(f"{c}:{idle.get(str(c), '')}" for c in CLIENT_CORE_LIST)
    per_thread = []
    for name, info in cpu.get("Per Thread", {}).items():
        per_thread.append((name.replace(" ", "_"), info.get("cores_used", "")))
    bound, bound_why = client_bound(per_thread, idle)
    row["client_bound"] = bound
    try:
        if float(row["connection_errors"]) != 0.0:
            reason.append("connection_errors_" + row["connection_errors"])
    except ValueError:
        reason.append("connection_errors_unparsed")
    if row["p50_ms"] == "" or row["p99_ms"] == "" or row["p99_9_ms"] == "":
        reason.append("missing_percentile")
    # A Redis error reply is not a connection error. memtier's own stdout
    # names that case when it happens.
    text = open(stdout_path, errors="replace").read()
    if "SERVER ERRORS" in text or "server error" in text.lower():
        reason.append("server_error_text")
    if not reason:
        row["valid"] = "1"
    # client_bound is not an error. Keep the note so the flag is explained,
    # and leave valid=1 when the replies themselves were clean.
    notes = list(reason)
    if bound == "1":
        notes.append("client_bound:" + bound_why)
    row["invalid_reason"] = ";".join(notes)
    append_row(row)
    return row


def main():
    resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))
    os.makedirs(RES, exist_ok=True)
    ensure_csv()
    log("results " + RES)
    done = load_done()
    limit = int(os.environ.get("MUTR_BENCH_LIMIT", "0"))
    # Optional "mix:size:conns:pipe" so a single cell can be checked
    # without writing the other cells. Unset for the full matrix.
    only = os.environ.get("MUTR_BENCH_WORK", "")
    ran = 0
    for rep in range(5):
        order = servers()[rep:] + servers()[:rep]
        for work in workloads():
            if only:
                want = (work["mix"], str(work["value_size"]), str(work["connections"]), str(work["pipeline"]))
                if want != tuple(only.split(":")):
                    continue
            for spec in order:
                if key_of(spec, work, rep) in done:
                    continue
                if limit and ran >= limit:
                    log("limit reached")
                    return 0
                label = (
                    f"rep={rep} {spec['server']} threads={spec['threads']} "
                    f"shards={spec['shards']} io={spec['redis_io_threads']} "
                    f"{work['mix']} {work['value_size']}B c={work['connections']} p={work['pipeline']}"
                )
                log("start " + label)
                err_path = os.path.join(RES, "server.err")
                proc = None
                try:
                    proc = start_server(spec, err_path)
                    row = measure(spec, work, rep, proc)
                    log(
                        "done valid=" + row["valid"] + " ops=" + row["ops_per_sec"]
                        + " bound=" + row["client_bound"] + " " + row["invalid_reason"]
                    )
                except Exception as exc:
                    log("FAIL " + label + " " + repr(exc))
                    append_row({
                        **{c: "" for c in COLUMNS},
                        "server": spec["server"], "threads": spec["threads"],
                        "shards": spec["shards"], "redis_io_threads": spec["redis_io_threads"],
                        "value_size": work["value_size"], "mix": work["mix"],
                        "connections": work["connections"], "pipeline": work["pipeline"],
                        "rep": rep, "valid": "0", "invalid_reason": "exception:" + repr(exc),
                        "client_bound": "0",
                    })
                finally:
                    stop(proc)
                ran += 1
                done.add(key_of(spec, work, rep))
    log("matrix complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
