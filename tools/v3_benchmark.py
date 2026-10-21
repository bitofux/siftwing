#!/usr/bin/env python3
"""正式服务的可复现本机闭环基准；只写全新目录，错误和原始样本均保留。"""
import argparse
from collections import defaultdict
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import resource
import select
import signal
import socket
import statistics
import string
import struct
import subprocess
import time

HEADER = struct.Struct("!4sBBHHHIQ")
KINDS = {1: "PING", 3: "SEARCH", 4: "SUGGEST"}


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(ok, why):
    if not ok:
        raise RuntimeError(why)


def command(directory, name, argv, timeout=60):
    started = time.monotonic()
    with (directory / (name + ".stdout")).open("wb") as out, (directory / (name + ".stderr")).open("wb") as err:
        result = subprocess.run(argv, stdout=out, stderr=err, timeout=timeout)
    receipt = {"argv": argv, "exit_code": result.returncode,
               "finished_at": datetime.datetime.now().astimezone().isoformat(),
               "wall_seconds": time.monotonic() - started}
    save(directory / (name + ".command.json"), receipt)
    check(result.returncode == 0, receipt)
    return receipt


def source_identity(root):
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root)
    names = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").decode().split("\0")
    return {"head": git("rev-parse", "HEAD").decode().strip(),
            "status": git("status", "--porcelain=v1", "--untracked-files=all").decode(),
            "source_sha256": {name: sha(root / name) for name in sorted(set(names))
                              if name and (root / name).is_file()}}


def environment():
    paths = ["/etc/os-release", "/proc/cpuinfo", "/proc/meminfo", "/proc/loadavg", "/proc/self/cgroup"]
    result = {"platform": platform.platform(), "uname": list(platform.uname()),
              "online_cpus": os.cpu_count(), "affinity": sorted(os.sched_getaffinity(0)),
              "files": {name: Path(name).read_text() for name in paths}, "visible_cgroups": {}}
    for line in result["files"]["/proc/self/cgroup"].splitlines():
        hierarchy, controllers, relative = line.split(":", 2)
        if hierarchy != "0" or controllers or ".." in Path(relative).parts:
            continue
        current = Path("/sys/fs/cgroup") / relative.lstrip("/")
        while current == Path("/sys/fs/cgroup") or Path("/sys/fs/cgroup") in current.parents:
            values = {}
            for field in ("cpu.max", "memory.max", "cpuset.cpus.effective"):
                try:
                    values[field] = (current / field).read_text().strip()
                except OSError as error:
                    values[field] = repr(error)
            result["visible_cgroups"][str(current)] = values
            if current == Path("/sys/fs/cgroup"):
                break
            current = current.parent
    return result


def exact(peer, count):
    data = b""
    while len(data) < count:
        part = peer.recv(count - len(data))
        check(bool(part), "truncated reference response")
        data += part
    return data


def exchange(peer, kind, identity, body):
    raw = json.dumps(body, separators=(",", ":")).encode()
    peer.sendall(HEADER.pack(b"SFTW", 1, 24, kind, 0, 0, len(raw), identity) + raw)
    header = HEADER.unpack(exact(peer, 24))
    check(header[:6] == (b"SFTW", 1, 24, kind + 0x8000, 0, 0) and header[-1] == identity,
          "reference type/status/id")
    check(2 <= header[6] <= 1048576, "reference body bound")
    answer = exact(peer, header[6]).decode("utf-8")
    parsed = json.loads(answer)
    check(isinstance(parsed, dict), "reference JSON object")
    check(answer == "{}" if kind == 1 else parsed["schema_version"] == 1
          and parsed["snapshot_version"] == "1" and parsed["command"] == KINDS[kind],
          "reference schema")
    if kind in (3, 4):
        check(bool(parsed["results"]), "nonempty synthetic query")
    return answer


def sample(pid):
    stat = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    status = dict(line.split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines())
    return {"time": time.monotonic(), "ticks": int(stat[11]) + int(stat[12]),
            "rss_kib": int(status["VmRSS"].split()[0]), "hwm_kib": int(status["VmHWM"].split()[0]),
            "threads": int(status["Threads"])}


class Server:
    """控制线程独占子进程；退出屏障后读取最终指标，不给服务增加观察接口。"""
    def __init__(self, options, directory, mode):
        self.directory = directory
        self.argv = [options.server, "--snapshot", str(options.output / "snapshot"), "--version", "1",
                     "--max-connections", "256", "--completion-capacity", "256", "--workers", "4", "--task-capacity", "128",
                     "--cache-capacity", str({"off": 0, "hot": 256, "evict": 16}[mode]),
                     "--cache-bytes", "8388608", "--cache-max-value-bytes", "262144",
                     "--log-level", "info", "--log-file", str(directory / "events.jsonl"),
                     "--log-max-bytes", "65536", "--log-max-files", "1", "--shutdown-ms", "1000"]
        save(directory / "server.argv.json", self.argv)
        save(directory / "server-topology.json", {
            "startup_schema_version": 2, "declared_topology": "single_reactor_worker_pool",
            "io_driver": "application_calling_thread", "workers": 4,
            "max_connections": 256, "completion_capacity": 256,
            "evidence_boundary": "启动配置身份；线程采样不能单独证明全部I/O归属",
        })
        self.error_file = (directory / "server.stderr").open("wb")
        self.process = subprocess.Popen(self.argv, stdout=subprocess.PIPE, stderr=self.error_file, bufsize=0)
        self.lines = []

    def __enter__(self):
        try:
            check(bool(select.select([self.process.stdout], [], [], 15)[0]), "server startup timeout")
            first = self.process.stdout.readline().decode().strip()
            self.lines.append(first)
            check(first.startswith("PORT "), ("server startup", first))
            self.port = int(first.split()[1])
            return self
        except BaseException as startup_error:
            self.__exit__(type(startup_error), startup_error, startup_error.__traceback__)
            raise

    def __exit__(self, error_type, error, traceback):
        try:
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
            try:
                output, _ = self.process.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                output, _ = self.process.communicate()
                (self.directory / "server.stdout").write_bytes(output)
                raise
            finally:
                self.error_file.close()
            self.lines += output.decode().splitlines()
            (self.directory / "server.stdout").write_text("\n".join(self.lines) + "\n")
            save(self.directory / "server.exit.json", {"argv": self.argv, "exit_code": self.process.returncode})
            check(self.process.returncode == 0 and not (self.directory / "server.stderr").stat().st_size,
                  "server exit/stderr")
            self.report = json.loads(self.lines[-1])
            check(self.report["active"] == 0 and self.report["workers_failed"] == 0, self.report)
            families = {}
            for path in self.directory.glob("events*.jsonl"):
                for line in path.read_text().splitlines():
                    event = json.loads(line)
                    if event.get("event") == "metrics_snapshot":
                        family = event["metric_family"]
                        if int(event["sample_id"]) >= int(families.get(family, {}).get("sample_id", -1)):
                            families[family] = event
            check("cache" in families and "shutdown" in families, "final metrics retained")
            check(families["shutdown"]["values"]["finished"], "shutdown barrier finished")
            self.cache = {key: int(value) for key, value in families["cache"]["values"].items()}
            save(self.directory / "final-metrics.json", families)
        except BaseException as cleanup_error:
            (self.directory / "cleanup-error.txt").write_text(repr(cleanup_error))
            if error_type is None:
                raise
        return False


def measure(options, server, directory, spec, connections, count=0, warmup=5):
    argv = [options.loadgen, str(server.port), str(connections), str(options.seconds),
            str(count), str(warmup), str(spec)]
    samples = [sample(server.process.pid)]
    started = time.monotonic()
    with (directory / "loadgen.stdout").open("wb") as out, (directory / "loadgen.stderr").open("wb") as err:
        process = subprocess.Popen(argv, stdout=out, stderr=err)
        try:
            while process.poll() is None:
                check(time.monotonic() - started < options.seconds + 30, "loadgen process deadline")
                time.sleep(.05)
                samples.append(sample(server.process.pid))
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    save(directory / "loadgen.command.json", {"argv": argv, "exit_code": process.returncode})
    save(directory / "resources.json", samples)
    check(process.returncode == 0 and not (directory / "loadgen.stderr").stat().st_size, "loadgen exit/stderr")
    report = json.loads((directory / "loadgen.stdout").read_text())
    check(report["successes"] == report["attempts"] > 0 and not report["errors"] and not report["setup_errors"],
          "every measured response admitted")
    check(sum(map(len, report["latency_ns_by_thread"])) == report["successes"], "complete raw samples")
    report.pop("latency_ns_by_thread")
    elapsed = samples[-1]["time"] - samples[0]["time"]
    report["server_cpu_percent_one_core"] = 100 * (samples[-1]["ticks"] - samples[0]["ticks"]) / os.sysconf("SC_CLK_TCK") / elapsed
    report["server_peak_rss_kib"] = max(row["rss_kib"] for row in samples)
    report["server_hwm_kib"] = max(row["hwm_kib"] for row in samples)
    return report


def prepare(options, head):
    rng = random.Random(20261016)
    vocabulary = ["".join(rng.choice(string.ascii_lowercase) for _ in range(8)) for _ in range(256)]
    check(len(set(vocabulary)) == 256, "unique generated vocabulary")
    items = []
    for i in range(options.documents):
        words = rng.sample(vocabulary, 40)
        items.append(f'<item><title>Record {i}</title><link>https://synthetic.example.invalid/{i}</link>'
                     + '<description>' + " ".join(words) + '</description></item>')
    text = ('<?xml version="1.0" encoding="UTF-8"?><rss version="2.0"><channel><title>Synthetic</title>'
            '<link>https://synthetic.example.invalid/</link><description>Deterministic benchmark</description>'
            + "".join(items) + '</channel></rss>')
    source = options.output / "synthetic.xml"
    source.write_text(text)
    weighted = [vocabulary[i % 4] for i in range(240)] + vocabulary[4:64]
    rng.shuffle(weighted)
    save(options.output / "queries.json", {"seed": 20261016, "unique": vocabulary[:64], "weighted_cycle": weighted})
    (options.output / "snapshot").mkdir(mode=0o700)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    receipt = command(options.output, "index", [options.indexer, "--output", str(options.output / "snapshot"),
                       "--version", "1", "--build-version", head, "--search-rss", str(source),
                       "--recommendation-rss", str(source), "--search-dedup-distance", "0"])
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    save(options.output / "dataset.json", {"seed": 20261016, "input_documents": options.documents,
         "input_bytes": source.stat().st_size, "input_sha256": sha(source), "index_command": receipt,
         "child_user_seconds": after.ru_utime - before.ru_utime, "child_system_seconds": after.ru_stime - before.ru_stime,
         "snapshot_sha256": {str(path.relative_to(options.output)): sha(path)
                             for path in (options.output / "snapshot").rglob("*") if path.is_file()}})
    directory = options.output / "reference"
    directory.mkdir(mode=0o700)
    with Server(options, directory, "off") as server:
        with socket.create_connection(("127.0.0.1", server.port), timeout=5) as peer:
            for kind in KINDS:
                bodies = [{}] if kind == 1 else [{"query" if kind == 3 else "key": word, "top_k": 5} for word in vocabulary[:64]]
                expected = [exchange(peer, kind, i + 1, body) for i, body in enumerate(bodies)]
                spec = {"kind": kind, "build_version": head, "bodies": bodies, "expected": expected}
                save(options.output / f"unique-{kind}.json", spec)
                if kind != 1:
                    mapping = {word: expected[i] for i, word in enumerate(vocabulary[:64])}
                    spec = dict(spec, bodies=[{"query" if kind == 3 else "key": word, "top_k": 5} for word in weighted],
                                expected=[mapping[word] for word in weighted])
                save(options.output / f"spec-{kind}.json", spec)


def summarize(options, records):
    groups = defaultdict(list)
    for row in records:
        groups[(row["phase"], row["command"], row["mode"], row["connections"])].append(row)
    summary = []
    for key, rows in sorted(groups.items()):
        values = [row["metrics"] for row in rows]
        item = dict(zip(("phase", "command", "mode", "connections"), key))
        item.update(repetitions=len(rows), qps_median=statistics.median(v["successful_qps"] for v in values),
                    qps_min=min(v["successful_qps"] for v in values), qps_max=max(v["successful_qps"] for v in values),
                    server_cpu_median=statistics.median(v["server_cpu_percent_one_core"] for v in values),
                    server_rss_max_kib=max(v["server_peak_rss_kib"] for v in values))
        for percentile in ("p50", "p95", "p99"):
            item[percentile + "_ms_median"] = statistics.median(v["latency_ms"][percentile] for v in values)
        summary.append(item)
    save(options.output / "summary.json", summary)
    with (options.output / "summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "indexer", "loadgen"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--documents", type=int, default=2000)
    parser.add_argument("--smoke", action="store_true", help="单轮短矩阵，仅验证工具流程")
    options = parser.parse_args()
    check(1 <= options.seconds <= 60 and 1 <= options.repeats <= 10 and 100 <= options.documents <= 100000,
          "bounded seconds/repeats/documents")
    options.output = options.output.resolve()
    options.output.mkdir(mode=0o700)
    for name in ("server", "indexer", "loadgen"):
        setattr(options, name, str(Path(getattr(options, name)).resolve(strict=True)))
    identity = source_identity(options.source_root)
    binaries = {name: {"path": getattr(options, name), "sha256": sha(Path(getattr(options, name)))}
                for name in ("server", "indexer", "loadgen")}
    save(options.output / "source-identity-before.json", identity)
    save(options.output / "binary-identity.json", binaries)
    save(options.output / "environment-before.json", environment())
    save(options.output / "options.json", {key: str(value) if isinstance(value, Path) else value for key, value in vars(options).items()})
    records = []
    try:
        prepare(options, identity["head"])
        connections = (1, 2) if options.smoke else (1, 8, 32)
        cases = [("steady", kind, mode, count) for kind in KINDS for mode in (("off",) if kind == 1 else ("off", "hot")) for count in connections]
        cases += [("steady", kind, "evict", 2 if options.smoke else 8) for kind in (3, 4)]
        cases += [("first64", kind, mode, 1) for kind in (3, 4) for mode in ("off", "hot")]
        rng = random.Random(20261016)
        for repetition in range(1, options.repeats + 1):
            order = cases[:]
            rng.shuffle(order)
            for phase, kind, mode, count in order:
                name = f"r{repetition}-{phase}-{KINDS[kind]}-{mode}-c{count}"
                directory = options.output / name
                directory.mkdir(mode=0o700)
                with Server(options, directory, mode) as server:
                    if phase == "steady" and kind != 1:
                        spec = json.loads((options.output / f"unique-{kind}.json").read_text())
                        with socket.create_connection(("127.0.0.1", server.port), timeout=5) as peer:
                            for i, body in enumerate(spec["bodies"]):
                                check(exchange(peer, kind, i + 1, body) == spec["expected"][i], "prewarm reference")
                    fixed = phase == "first64"
                    metrics = measure(options, server, directory, options.output / f'{"unique" if fixed else "spec"}-{kind}.json',
                                      count, count=64 if fixed else 0, warmup=0 if fixed else 5)
                cache = server.cache
                if kind != 1 and mode == "hot":
                    check(cache["misses"] == 64 and cache["evictions"] == 0, ("known complete warm/cold working set", cache))
                    check(cache["hits"] == (0 if fixed else metrics["successes"] + 5 * count), "actual cache hits")
                if mode == "off":
                    check(cache["hits"] == cache["misses"] == cache["evictions"] == 0, "cache bypass")
                if mode == "evict":
                    check(cache["evictions"] > 0 and cache["entries"] <= 16, "real eviction workload")
                record = {"name": name, "phase": phase, "command": KINDS[kind], "mode": mode,
                          "connections": count, "repetition": repetition, "metrics": metrics, "cache": cache}
                records.append(record)
                save(options.output / "results.json", records)
                print(name, "QPS", round(metrics["successful_qps"], 1), "p99_ms", round(metrics["latency_ms"]["p99"], 3), flush=True)
        check(source_identity(options.source_root) == identity, "source changed during benchmark")
        check(all(sha(Path(value["path"])) == value["sha256"] for value in binaries.values()), "binary changed")
        dataset = json.loads((options.output / "dataset.json").read_text())
        check(sha(options.output / "synthetic.xml") == dataset["input_sha256"], "input changed")
        check({str(path.relative_to(options.output)): sha(path)
               for path in (options.output / "snapshot").rglob("*") if path.is_file()}
              == dataset["snapshot_sha256"], "snapshot input changed")
        summarize(options, records)
        save(options.output / "source-identity-after.json", source_identity(options.source_root))
        save(options.output / "environment-after.json", environment())
        save(options.output / "complete.json", {"windows": len(records), "successes": sum(row["metrics"]["successes"] for row in records), "errors": 0})
    except BaseException:
        print("失败原件保留：", options.output, flush=True)
        raise


if __name__ == "__main__":
    main()
