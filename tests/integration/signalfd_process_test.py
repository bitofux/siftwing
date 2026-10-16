#!/usr/bin/env python3
"""正式入口及仅链接注入副本的线程mask、启动失败和关闭pending窗口。"""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import cli_test as ref


def check(ok, reason):
    if not ok:
        raise AssertionError(reason)


def main():
    parser = argparse.ArgumentParser()
    for name in ("server", "probe", "indexer", "writer"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    ref.OPTIONS = args
    root = Path(tempfile.mkdtemp(prefix="siftwing-signalfd-process-"))
    try:
        parent, _, _ = ref.golden(root)
        before = ref.inventory(root)
        required = (1 << (signal.SIGINT - 1)) | (1 << (signal.SIGTERM - 1))
        # 真实生产程序：所有已启动线程保留受管mask，未安装旧handler；两种信号正常退出。
        for signo in (signal.SIGTERM, signal.SIGINT):
            proc = subprocess.Popen([args.server, "--snapshot", str(parent), "--version", "0"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                check(select.select([proc.stdout], [], [], 20)[0], "readiness timeout")
                check(proc.stdout.readline().startswith(b"PORT "), "production readiness")
                tasks = list(Path(f"/proc/{proc.pid}/task").glob("*/status"))
                check(len(tasks) == 7, "Main plus two Sub and four workers")
                for status in tasks:
                    fields = dict(line.split(":", 1) for line in status.read_text().splitlines() if ":" in line)
                    check(int(fields["SigBlk"], 16) & required == required, "all threads inherit termination mask")
                    check(int(fields["SigCgt"], 16) & required == 0, "no managed signal handler installed")
                proc.send_signal(signo)
                out, err = proc.communicate(timeout=20)
                check(proc.returncode == 0 and json.loads(out)["active"] == 0, "normal production signal shutdown")
                events = [json.loads(line)["event"] for line in err.splitlines()]
                check("server_stopped" in events and "server_failed" not in events, "normal signal log semantics")
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate(timeout=5)
        # 测试副本仍编译同一main.cc；只在系统边界注入，不增加生产环境开关。
        for mode, expected in (("startup_term", 0), ("close_pending", 0),
                               ("block_failure", 1), ("create_failure", 1), ("add_failure", 1)):
            env = dict(os.environ, SIFTWING_TEST_SIGNAL_MODE=mode)
            result = subprocess.run([args.probe, "--snapshot", str(parent), "--version", "0",
                                     "--run-ms", "0" if mode == "startup_term" else "5"], env=env, capture_output=True, timeout=20)
            check(result.returncode == expected, (mode, result.returncode, result.stderr))
            if expected == 0:
                lines = result.stdout.splitlines()
                check(lines[0].startswith(b"PORT ") and json.loads(lines[1])["active"] == 0,
                      "pending signals preserve normal exit report")
                check(all(json.loads(row)["event"] != "server_failed" for row in result.stderr.splitlines()),
                      "pending termination not failure")
            else:
                check(not result.stdout, "failed startup never announces PORT")
                if mode == "block_failure":
                    check(b"errno=11" in result.stderr, "pthread return code retained instead of stale errno")
        check(ref.inventory(root) == before, "original fixture unchanged")
        print("signalfd production mask/startup/close windows passed")
    except BaseException:
        print(f"失败原件保留：{root}")
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
