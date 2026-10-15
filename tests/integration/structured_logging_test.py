#!/usr/bin/env python3
"""独立Python解析真实服务日志，验证四命令、精确缓存、隐私及坏目标业务继续。"""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import cli_test as ref
import online_search_test as online


def check(value, label):
    if not value:
        raise AssertionError(label)


def run(root, parent, records, frequencies, mode):
    path = root / (mode + ".jsonl")
    extra = ["--cache-capacity", "16", "--idle-timeout-ms", "1000"]
    default_level = mode.startswith("default") or mode == "off"
    if not default_level:
        extra += ["--log-level", "debug"]
    elif mode == "off":
        extra += ["--log-level", "off"]
    stderr = subprocess.PIPE
    owned = None
    file_mode = mode in ("file", "default-file")
    if file_mode:
        extra += ["--log-file", str(path)]
    elif mode in ("stderr", "default", "off"):
        owned = open(root / "stderr-capture", "w+b")
        stderr = owned
    elif mode == "full":
        owned = open("/dev/full", "wb")
        stderr = owned
    elif mode == "pipe":
        reader, writer = os.pipe()
        os.close(reader)
        owned = os.fdopen(writer, "wb")
        stderr = owned
    # 文件配置与CLI共同覆盖日志字段，off文件组合由下面的负例验证。
    config = root / (mode + ".config")
    values = dict(schema_version=1, snapshot=str(parent), version="0")
    if not default_level:
        values["log_level"] = "info"
    config.write_text(json.dumps(values), encoding="utf8")
    proc = subprocess.Popen([OPTIONS.server, "--config", str(config), *extra],
                            stdout=subprocess.PIPE, stderr=stderr)
    line = b""
    try:
        check(select.select([proc.stdout], [], [], 20)[0], "启动超时")
        line = proc.stdout.readline()
        check(line.startswith(b"PORT "), (mode, line, proc.poll()))
        with socket.create_connection(("127.0.0.1", int(line.split()[1])), timeout=5) as peer:
            peer.settimeout(5)
            peer.sendall(online.wire(1, 7, {}))
            check(online.read(peer, 0x8001, 7) == {}, "PING")
            peer.sendall(online.wire(2, 1, {}))
            check(online.read(peer, 0x8002, 1)["snapshot_version"] == "0", "INFO")
            first = online.search(peer, 2**64-1, records, "red")
            check(online.search(peer, 3, records, "red") == first, "相同查询独立参考")
            suggest = online.suggest(peer, 4, frequencies, "cat")
            peer.sendall(online.wire(3, 5, {"query": "PRIVATE-QUERY-SECRET", "unknown": True}))
            online.read(peer, 0xFFFF, 5, 1)
            # 完整响应后由空闲期限主动关连接；业务错误后仍可处理PING。
            peer.sendall(online.wire(1, 6, {}))
            online.read(peer, 0x8001, 6)
            check(peer.recv(1) == b"", "空闲期限关闭")
        with socket.create_connection(("127.0.0.1", int(line.split()[1])), timeout=5) as invalid:
            invalid.settimeout(5)
            packet = online.wire(1, 8, {})
            invalid.sendall(b"FAIL" + packet[4:])
            check(invalid.recv(1) == b"", "非法头局部关闭")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            out, err = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise
        if mode in ("stderr", "default", "off") and owned:
            owned.seek(0)
            err = owned.read()
        if owned:
            owned.close()
        (root / (mode + ".stdout")).write_bytes(line + out)
        if err is not None:
            (root / (mode + ".stderr")).write_bytes(err)
    check(proc.returncode == 0 and json.loads(out)["active"] == 0,
          (mode, "完整停止", proc.returncode, out, err))
    if mode in ("full", "pipe"):
        return  # 真实业务结果已用独立参考逐一核对；坏日志目标不改变退出/响应。
    data = path.read_bytes() if file_mode else err
    check(not file_mode or not err, "文件模式stderr无混入")
    if mode == "off":
        check(data == b"", "显式off关闭运行日志且业务继续")
        return
    check(data.endswith(b"\n") and b"PRIVATE" not in data and str(parent).encode() not in data,
          "JSON行与隐私")
    rows = [json.loads(line) for line in data.splitlines()]
    expected_fields = {"schema_version", "time_unix_us", "level", "thread_id", "module", "event", "code",
                       "reactor_id", "connection_id", "generation", "request_id", "index_version",
                       "elapsed_us", "result_count", "cache_state", "native_error"}
    for row, raw in zip(rows, data.splitlines()):
        check(set(row) == expected_fields and row["schema_version"] == 1 and len(raw) + 1 <= 2048,
              "固定schema与行预算")
        check(row["time_unix_us"].isdigit() and row["thread_id"].isdigit(), "时间和实际线程")
    events = {row["event"] for row in rows}
    if default_level:
        check({"server_starting", "server_started", "server_stopped", "connection_opened",
               "connection_closed", "idle_expired"} <= events, "未指定级别仍记录生命周期与异常")
        check({row["level"] for row in rows} == {"info", "warn"}, "默认info过滤debug并保留warn")
        check(not events.intersection({"request_received", "task_submitted", "cache_lookup",
                                       "completion_published", "completion_applied"}),
              "默认不输出逐请求debug事件")
        return
    check({"server_starting", "server_started", "server_stopped", "connection_opened", "connection_closed",
           "request_received", "task_submitted", "cache_lookup", "query_finished", "completion_published",
           "completion_applied", "idle_expired"} <= events, "真实阶段完整")
    received = [row for row in rows if row["event"] == "request_received"]
    check(len(received) == 7, "全部四命令和错误受信请求")
    identity = lambda row: (row["reactor_id"], row["connection_id"], row["generation"])
    check(len({identity(row) for row in received}) == 1 and received[0]["request_id"] == "7",
          "全链路身份与请求关联")
    for request, cache, result in [(str(2**64-1), "miss", first), ("3", "hit", first), ("4", "miss", suggest)]:
        query = [row for row in rows if row["event"] == "query_finished" and row["request_id"] == request]
        check(len(query) == 1, "每次查询完成一次")
        row = query[0]
        check(identity(row) == identity(received[0]) and row["index_version"] == "0"
              and row["cache_state"] == cache and row["result_count"] == len(result["results"])
              and isinstance(row["elapsed_us"], int) and row["elapsed_us"] >= 0,
              "精确缓存、结果长度、耗时和版本")
        check(row["thread_id"] != received[0]["thread_id"], "worker与Sub线程分离")
    protocol_closes = [row for row in rows if row["event"] == "connection_closed" and row["code"] == "protocol_error"]
    check(len(protocol_closes) == 1 and protocol_closes[0]["level"] == "warn"
          and protocol_closes[0]["request_id"] is None, "非法头关闭有稳定原因且不猜请求号")
    failures = [row for row in rows if row["event"] == "query_finished" and row["request_id"] == "5"]
    check(len(failures) == 1 and failures[0]["level"] == "warn"
          and failures[0]["code"] == "business_error" and failures[0]["cache_state"] is None,
          "校验失败不冒充缓存miss")


def rotation(root, parent):
    # 用真实CLI和多轮PING触发淘汰，独立验证文件集合、大小、完整JSON与最后生命周期。
    path = root / "rotating.jsonl"
    proc = subprocess.Popen([OPTIONS.server, "--snapshot", str(parent), "--version", "0",
                             "--log-level", "debug", "--log-file", str(path),
                             "--log-max-bytes", "2048", "--log-max-files", "2"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = b""
    try:
        check(select.select([proc.stdout], [], [], 20)[0], "轮转服务启动期限")
        line = proc.stdout.readline()
        check(line.startswith(b"PORT "), "轮转服务发布PORT")
        with socket.create_connection(("127.0.0.1", int(line.split()[1])), timeout=5) as peer:
            peer.settimeout(5)
            for identity in range(1001, 1121):
                peer.sendall(online.wire(1, identity, {}))
                check(online.read(peer, 0x8001, identity) == {}, "轮转期间PING业务继续")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            out, err = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise
        (root / "rotation.stdout").write_bytes(line + out)
        (root / "rotation.stderr").write_bytes(err)
    check(proc.returncode == 0 and not err and json.loads(out)["responses"] == 120,
          "真实CLI轮转不影响响应/退出")
    expected = {"rotating.jsonl", "rotating.1.jsonl", "rotating.2.jsonl"}
    paths = list(root.glob("rotating*.jsonl"))
    check({p.name for p in paths} == expected, "当前文件加两份归档")
    for candidate in paths:
        data = candidate.read_bytes()
        check(data.endswith(b"\n") and len(data) <= 2048, "轮转大小有界且末行完整")
        check(candidate.stat().st_mode & 0o777 == 0o600, "轮转文件0600")
        check(all(json.loads(raw)["schema_version"] == 1 for raw in data.splitlines()), "轮转JSON可解析")
    check(json.loads(path.read_bytes().splitlines()[-1])["event"] == "server_stopped",
          "最新文件保留最终生命周期")


def main():
    root = Path(tempfile.mkdtemp(prefix="siftwing-structured-logging-"))
    print("测试现场：", root, flush=True)
    parent, records, frequencies = ref.golden(root)
    for mode in ("file", "stderr", "default", "default-file", "off", "full", "pipe"):
        run(root, parent, records, frequencies, mode)
    rotation(root, parent)
    # 配置错误与新文件初始化失败必须发生在服务装载/监听前。
    existing = root / "existing"
    existing.write_text("UNCHANGED")
    cases = [(["--log-level", "verbose"], 2),
             (["--log-level", "off", "--log-file", str(existing)], 2),
             (["--log-file", str(existing)], 1),
             (["--log-level", "info", "--log-file", str(existing)], 1)]
    for extra, expected in cases:
        result = subprocess.run([OPTIONS.server, "--snapshot", "missing", "--version", "0", *extra],
                                capture_output=True, timeout=10)
        check(result.returncode == expected and not result.stdout, "日志准入先于服务资源")
    check(existing.read_text() == "UNCHANGED", "既有文件保持")
    missing = str(root / "PRIVATE-ABSOLUTE-SNAPSHOT")
    result = subprocess.run([OPTIONS.server, "--snapshot", missing, "--version", "0"],
                            capture_output=True, timeout=10)
    failure_rows = [json.loads(line) for line in result.stderr.splitlines()]
    check(result.returncode == 1 and not result.stdout and missing.encode() not in result.stderr
          and [row["event"] for row in failure_rows] == ["server_starting", "server_failed"]
          and failure_rows[-1]["code"] == "service_load", "装载失败固定安全日志")
    print("真实日志与失败隔离通过；现场保留", root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("server", "indexer", "writer"):
        parser.add_argument("--" + name, required=True)
    OPTIONS = parser.parse_args()
    ref.OPTIONS = OPTIONS
    main()
