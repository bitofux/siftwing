#!/usr/bin/env python3
"""原创伪服务验证发生器的响应准入、失败计数和分位口径，不测服务性能。"""
import argparse
import json
import math
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading

HEADER = struct.Struct("!4sBBHHHIQ")


def exact(peer, length):
    data = b""
    while len(data) < length:
        part = peer.recv(length - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


def check(ok, why):
    if not ok:
        raise RuntimeError(why)


def case(native, root, mode):
    count = 2 if mode == "valid" else 1
    listener = socket.socket()
    listener.settimeout(8)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    port = listener.getsockname()[1]
    errors = []

    def respond(peer):
        try:
            with peer:
                peer.settimeout(8)
                while True:
                    request = HEADER.unpack(exact(peer, 24))
                    exact(peer, request[6])
                    kind, identity = request[3], request[-1]
                    body = json.dumps({"command": "SERVER_INFO", "schema_version": 1,
                                       "snapshot_version": "1", "protocol_version": 1,
                                       "build_version": "wrong" if mode == "build" else "selftest"},
                                      separators=(",", ":")).encode() if kind == 2 else b"{}"
                    header = [b"SFTW", 1, 24, kind + 0x8000, 0, 0, len(body), identity]
                    if kind == 1:
                        if mode == "id":
                            header[-1] += 1
                        if mode == "magic":
                            header[0] = b"FAIL"
                        if mode == "version":
                            header[1] = 2
                        if mode == "hsize":
                            header[2] = 23
                        if mode == "flags":
                            header[4] = 1
                        if mode == "status":
                            header[5] = 1
                        if mode == "type":
                            header[3] = 0x8004
                        if mode == "length":
                            header[6] = 1048577
                        if mode == "json":
                            body = b"\xff?"
                        if mode == "pong":
                            body = b"[]"
                        if mode == "remote_error":
                            header[3], header[5] = 65535, 6
                        if mode == "eof":
                            return
                        if mode == "truncated":
                            peer.sendall(HEADER.pack(*header) + b"{")
                            return
                        if mode == "timeout":
                            peer.sendall(HEADER.pack(*header) + b"{")
                            peer.recv(1)  # 少量进展不延长5秒总期限，等发生器自行关闭。
                            return
                    packet = HEADER.pack(*header) + body
                    if mode == "fragmented":
                        for piece in (packet[:1], packet[1:4], packet[4:11], packet[11:]):
                            peer.sendall(piece)
                    else:
                        peer.sendall(packet)
        except (EOFError, ConnectionError):
            pass
        except BaseException as error:
            errors.append(repr(error))

    def serve():
        peers = []
        try:
            for _ in range(count):
                thread = threading.Thread(target=respond, args=(listener.accept()[0],), daemon=True)
                peers.append(thread)
                thread.start()
            for thread in peers:
                thread.join(9)
                check(not thread.is_alive(), "fixture thread joined")
        except BaseException as error:
            errors.append(repr(error))
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    argv = [native, str(port), str(count), "1", "3", "0", str(root / "spec.json")]
    result = subprocess.run(argv, capture_output=True, timeout=15)
    (root / (mode + ".stdout")).write_bytes(result.stdout)
    (root / (mode + ".stderr")).write_bytes(result.stderr)
    thread.join(10)
    check(not thread.is_alive() and not errors, (mode, errors))
    report = json.loads(result.stdout)
    good = mode in ("valid", "fragmented")
    check(result.returncode == (0 if good else 1) and not result.stderr, (mode, result.returncode))
    check(report["successes"] == (3 * count if good else 0), (mode, report))
    check(report["attempts"] == (3 * count if good else 0 if mode == "build" else 1), mode)
    check(bool(report["setup_errors"]) == (mode == "build"), mode)
    check(bool(report["errors"]) == (not good and mode != "build"), mode)
    samples = sorted(value for row in report["latency_ns_by_thread"] for value in row)
    check(len(samples) == report["successes"], "raw samples equal admitted successes")
    if good:
        check(report["response_wire_bytes"] == 26 * len(samples), "only measured responses counted")
        check(math.isclose(report["successful_qps"], len(samples) / report["elapsed_seconds"]),
              "actual elapsed denominator")
        for name, fraction in (("p50", .5), ("p95", .95), ("p99", .99)):
            check(report["latency_ms"][name] == samples[math.ceil(len(samples) * fraction) - 1] / 1e6,
                  "nearest-rank from raw samples")
    return {"mode": mode, "exit_code": result.returncode, "successes": report["successes"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="siftwing-benchmark-contract-"))
    (root / "spec.json").write_text(json.dumps({"kind": 1, "bodies": [{}], "expected": ["{}"],
                                               "build_version": "selftest"}))
    try:
        modes = ("valid", "fragmented", "id", "magic", "version", "hsize", "flags", "status",
                 "type", "length", "json", "pong", "eof", "truncated", "remote_error", "timeout", "build")
        results = [case(args.native, root, mode) for mode in modes]
        print(json.dumps({"cases": len(results), "results": results}))
    except BaseException:
        print("失败原件保留：", root, flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
