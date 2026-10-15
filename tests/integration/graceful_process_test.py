#!/usr/bin/env python3
"""真实indexer/server进程的信号/定时关闭与严格参数；无需私有输入或时序猜测。"""
import argparse
import json
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile

import cli_test as reference

CHECKS = 0


def check(value, reason):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(reason)


def run(options, parent, extra, stop_signal=None):
    # 关闭测试保留空stderr断言；默认日志的完整停止路径由structured_logging验证。
    command = [options.server, "--log-level", "off", "--snapshot", str(parent), "--version", "0", *extra]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    peers = []
    try:
        check(select.select([process.stdout], [], [], 20)[0], "startup timeout")
        line = process.stdout.readline()
        check(line.startswith(b"PORT "), ("server readiness", line))
        port = int(line.split()[1])
        # 完整PING/PONG往返确认应用服务可用，随后保留空闲与半包连接进入关闭。
        peer = socket.create_connection(("127.0.0.1", port), timeout=5)
        peers.append(peer)
        request = struct.pack("!4sBBHHHIQ", b"SFTW", 1, 24, 1, 0, 0, 2, 77) + b"{}"
        peer.sendall(request)
        expected = struct.pack("!4sBBHHHIQ", b"SFTW", 1, 24, 0x8001, 0, 0, 2, 77) + b"{}"
        actual = b""
        while len(actual) < len(expected):
            part = peer.recv(len(expected) - len(actual))
            check(bool(part), "complete PONG before shutdown")
            actual += part
        check(actual == expected, "independent golden PONG")
        partial = socket.create_connection(("127.0.0.1", port), timeout=5)
        peers.append(partial)
        partial.sendall(request[:10])
        if stop_signal is not None:
            process.send_signal(stop_signal)
        output, error = process.communicate(timeout=20)
        check(process.returncode == 0 and not error, ("normal process shutdown", process.returncode, error))
        report = json.loads(output)
        check(report["active"] == 0 and report["workers_failed"] == 0 and report["responses"] >= 1,
              "final network and worker report")
        for connection in peers:
            try:
                check(connection.recv(1) == b"", "shutdown closes idle/partial peer without response")
            except ConnectionResetError:
                check(True, "unread half packet may reset peer")
        with socket.socket() as newcomer:
            check(newcomer.connect_ex(("127.0.0.1", port)) != 0, "listener no longer accepts")
    finally:
        for peer in peers:
            peer.close()
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    for name in ("server", "indexer", "writer"):
        parser.add_argument("--" + name, required=True)
    options = parser.parse_args()
    reference.OPTIONS = options
    root = Path(tempfile.mkdtemp(prefix="siftwing-graceful-process-"))
    try:
        parent, _, _ = reference.golden(root)
        before = reference.inventory(root)
        help_result = subprocess.run([options.server, "--help"], capture_output=True, timeout=10)
        check(help_result.returncode == 0 and b"--shutdown-ms" in help_result.stdout,
              "help describes network drain option")
        for arguments, stop_signal in [([], signal.SIGTERM), (["--shutdown-ms", "0"], signal.SIGINT),
                                       (["--shutdown-ms", "86400000"], signal.SIGTERM),
                                       (["--run-ms", "1000", "--shutdown-ms", "20"], None)]:
            run(options, parent, arguments, stop_signal)
        for value in ("-1", "86400001", "01", "+1", "1.0", "", "18446744073709551616"):
            result = subprocess.run([options.server, "--snapshot", str(parent), "--version", "0",
                                     "--shutdown-ms", value], capture_output=True, timeout=10)
            check(result.returncode == 2 and not result.stdout and result.stderr, ("invalid shutdown option", value))
        for tail in (["--shutdown-ms"], ["--shutdown-ms", "1", "--shutdown-ms", "2"]):
            result = subprocess.run([options.server, "--snapshot", str(parent), "--version", "0", *tail],
                                    capture_output=True, timeout=10)
            check(result.returncode == 2 and not result.stdout and result.stderr, "missing/duplicate option")
        check(reference.inventory(root) == before, "original fixture and snapshot remain unchanged")
        print(f"{CHECKS} graceful process checks passed")
    except BaseException:
        print(f"失败夹具保留于：{root}")
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
