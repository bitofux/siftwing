#!/usr/bin/env python3
"""V2 小型串行 loopback 基准；逐帧准入，错误不计作成功吞吐。"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import socket
import struct
import sys
import time

KINDS = {"PING": (1, 0x8001), "SEARCH": (3, 0x8003), "SUGGEST": (4, 0x8004)}


def integer(low, high):
    def parse(value):
        try:
            result = int(value)
        except ValueError as error:
            raise argparse.ArgumentTypeError("需要整数") from error
        if not low <= result <= high:
            raise argparse.ArgumentTypeError(f"范围为 {low}..{high}")
        return result
    return parse


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("响应 JSON 有重复字段")
        result[key] = value
    return result


def finite_number(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("响应 JSON 数值不可表示")
    return result


def bad_constant(value):
    raise ValueError(f"非法 JSON 常量：{value}")


def exact(peer, size, deadline):
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("一次交换总期限到达")
        peer.settimeout(remaining)
        part = peer.recv(size - len(data))
        if not part:
            raise ValueError("响应截断")
        data.extend(part)
    return bytes(data)


def exchange(peer, kind, expected, identity, body, timeout):
    # 期限覆盖编码、发送、完整接收与准入；少量进展不续期。
    deadline = time.monotonic() + timeout
    payload = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if not 2 <= len(payload) <= 1048576:
        raise ValueError("请求 Body 超限")
    packet = struct.pack("!4sBBHHHIQ", b"SFTW", 1, 24, kind, 0, 0, len(payload), identity) + payload
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("请求编码期限到达")
    peer.settimeout(remaining)
    peer.sendall(packet)
    header = struct.unpack("!4sBBHHHIQ", exact(peer, 24, deadline))
    magic, version, size, actual, flags, status, length, correlation = header
    if (magic, version, size, actual, flags, status, correlation) != (
            b"SFTW", 1, 24, expected, 0, 0, identity):
        raise ValueError(f"响应类型、关联号或状态非法：{header}")
    if not 2 <= length <= 1048576:
        raise ValueError("响应 Body 超限")
    raw = exact(peer, length, deadline)
    result = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object,
                        parse_float=finite_number, parse_constant=bad_constant)
    if not isinstance(result, dict) or (kind == 1 and raw != b"{}"):
        raise ValueError("响应不是 JSON 对象或 PONG 非固定 Body")
    if time.monotonic() >= deadline:
        raise TimeoutError("响应准入总期限到达")
    return result, 24 + length


def percentile(samples, fraction):
    # nearest-rank；小样本尾分位仅描述本次观测。
    return sorted(samples)[max(0, math.ceil(len(samples) * fraction) - 1)] / 1e6


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=integer(1, 65535))
    parser.add_argument("--command", choices=KINDS, default="SEARCH")
    parser.add_argument("--query", default="lantern")
    parser.add_argument("--key", default="lantern")
    parser.add_argument("--requests", type=integer(1, 100000), default=200)
    parser.add_argument("--warmup", type=integer(0, 10000), default=10)
    parser.add_argument("--timeout-ms", type=integer(1, 60000), default=5000)
    options = parser.parse_args()
    timeout = options.timeout_ms / 1000
    kind, expected = KINDS[options.command]
    body = {} if kind == 1 else ({"query": options.query, "top_k": 5} if kind == 3
                                else {"key": options.key, "top_k": 5})
    # 只有一个持久连接、一个在途请求；连接/INFO/预热不进入计量窗口。
    with socket.create_connection(("127.0.0.1", options.port), timeout=timeout) as peer:
        info, _ = exchange(peer, 2, 0x8002, 1, {}, timeout)
        if (info.get("command") != "SERVER_INFO" or info.get("schema_version") != 1
                or info.get("protocol_version") != 1
                or not isinstance(info.get("snapshot_version"), str)):
            raise ValueError("SERVER_INFO 不符合基准前置合同")
        latencies, response_bytes = [], 0
        started = None
        for offset in range(options.warmup + options.requests):
            if offset == options.warmup:
                started = time.perf_counter_ns()
            begin = time.perf_counter_ns()
            result, count = exchange(peer, kind, expected, offset + 2, body, timeout)
            if kind != 1 and (result.get("command") != options.command
                              or result.get("schema_version") != 1
                              or result.get("snapshot_version") != info["snapshot_version"]
                              or not isinstance(result.get("results"), list)):
                raise ValueError("业务响应与当前命令/快照不符")
            elapsed = time.perf_counter_ns() - begin
            if offset >= options.warmup:
                latencies.append(elapsed)
                response_bytes += count
        seconds = (time.perf_counter_ns() - started) / 1e9
    report = {
        "schema_version": 1, "scope": "loopback_serial_observation",
        "command": options.command, "request_body": body, "port": options.port,
        "requests": options.requests, "warmup": options.warmup, "connections": 1,
        "max_inflight": 1, "timeout_ms": options.timeout_ms,
        "elapsed_seconds": seconds, "responses_per_second": options.requests / seconds,
        "latency_ms": {"p50": percentile(latencies, .50), "p95": percentile(latencies, .95),
                       "p99": percentile(latencies, .99)},
        "response_wire_bytes": response_bytes, "latency_ns": latencies, "server_info": info,
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "environment": {"platform": platform.platform(), "python": platform.python_version(),
                        "machine": platform.machine()},
    }
    print(json.dumps(report, ensure_ascii=False, allow_nan=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"V2 基准失败：{error}", file=sys.stderr)
        sys.exit(1)
