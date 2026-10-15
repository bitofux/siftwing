#!/usr/bin/env python3
"""配置驱动真实服务，手写帧及独立排名/推荐参考；失败目录保留。

验证cwd路径、启动后文件独立、覆盖顺序、空闲期限、显式IPv6与未发布前拒绝。
线程/缓存的内部命中计数不由网络结果推断，原有缓存单元和并发测试继续负责。
"""
import argparse
import json
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import cli_test as ref
import online_search_test as online


def check(ok, label):
    if not ok:
        raise AssertionError(label)


def run(root, host="127.0.0.1"):
    parent, records, frequencies = ref.golden(root)
    config = root / "server.json"
    values = dict(schema_version=1, snapshot=parent.relative_to(root).as_posix(), version="0",
                  listen_address=host, port=1, subs=1, workers=1, task_capacity=8,
                  handoff_capacity=8, completion_capacity=8, max_connections=8,
                  cache_capacity=16, cache_bytes=8192, cache_max_value_bytes=4096,
                  idle_timeout_ms=300, request_timeout_ms=5000, shutdown_ms=1000,
                  tcp_no_delay=True, run_ms=1)
    config.write_text(json.dumps(values), encoding="utf-8")
    # --port/--run-ms在文件前后均覆盖；run_ms=1若未覆盖会提前退出。
    # 本夹具保持空stderr的配置合同；默认info的真实进程接线另有日志集成测试。
    process = subprocess.Popen([OPTIONS.server, "--log-level", "off", "--port", "0", "--config", "server.json", "--run-ms", "0"],
                               cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        check(select.select([process.stdout], [], [], 20)[0], "启动期限")
        line = process.stdout.readline()
        check(line.startswith(b"PORT "), (line, process.poll()))
        port = int(line.split()[1])
        # 配置只在启动读取；删除自己的临时文件后业务仍工作。
        config.unlink()
        with socket.create_connection((host, port), timeout=5) as peer:
            peer.settimeout(5)
            peer.sendall(online.wire(1, 1, {}))
            check(online.read(peer, 0x8001, 1) == {}, "PING")
            peer.sendall(online.wire(2, 2, {}))
            info = online.read(peer, 0x8002, 2)
            check(info["snapshot_version"] == "0" and info["cache_enabled"] is True,
                  "SERVER_INFO快照版本与配置启用缓存")
            first = online.search(peer, 3, records, "red")
            check(online.search(peer, 4, records, "red") == first, "缓存前后独立数学参考相同")
            online.suggest(peer, 5, frequencies, "cat")
        with socket.create_connection((host, port), timeout=5) as idle:
            idle.settimeout(5)
            check(idle.recv(1) == b"", "配置空闲期限真实关闭连接")
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            out, err = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        (root / "server.stdout").write_bytes(line + out if 'line' in locals() else out)
        (root / "server.stderr").write_bytes(err)
    check(process.returncode == 0 and not err and json.loads(out)["active"] == 0,
          ("完整停止", process.returncode, out, err))
    # 生产入口证明错误配置在快照装载/监听之前拒绝，非法文件不受CLI掩盖。
    for body, extra, context in [
            ('{"schema_version":1,"workers":0}', ["--workers", "1"], b"config.value"),
            ('{"schema_version":1,"workers":1,"workers":2}', [], b"config.json"),
            ('{"schema_version":1,"snapshot":"missing","version":"0","cache_bytes":1}', [], b"config.value"),
            ('{"schema_version":1}', [], b"config.required")]:
        config.write_text(body)
        result = subprocess.run([OPTIONS.server, "--config", "server.json", *extra], cwd=root,
                                capture_output=True, timeout=5)
        check(result.returncode == 2 and result.stdout == b"" and context in result.stderr,
              ("配置失败无PORT", result.returncode, result.stdout, result.stderr))
    config.write_text(json.dumps(dict(schema_version=1, snapshot="missing", version="0")))
    result = subprocess.run([OPTIONS.server, "--config", "server.json"], cwd=root, capture_output=True, timeout=5)
    check(result.returncode == 1 and not result.stdout, "合法配置但装载失败保持exit1")
    with open("/dev/full", "wb") as full:
        result = subprocess.run([OPTIONS.server, "--help"], stdout=full, stderr=subprocess.PIPE, timeout=5)
    check(result.returncode == 3, "帮助输出失败")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ["server", "writer", "indexer"]:
        parser.add_argument("--" + name, required=True)
    OPTIONS = parser.parse_args()
    ref.OPTIONS = OPTIONS
    root = Path(tempfile.mkdtemp(prefix="siftwing-config-process-"))
    try:
        run(root)
        # Ubuntu验证环境应支持loopback IPv6，失败如实暴露，不静默跳过。
        ipv6 = root / "ipv6"
        ipv6.mkdir()
        run(ipv6, "::1")
    except BaseException:
        print("失败原创夹具保留：", root)
        raise
    else:
        shutil.rmtree(root)
    print(f"config process checks: wire={online.CHECKS} fixture={ref.CHECKS} failures=0")
