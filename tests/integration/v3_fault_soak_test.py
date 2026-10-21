#!/usr/bin/env python3
"""独立原创成品驱动故障/有界持续测试；隔离进程软限，失败保留输入与原始输出。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from reader_reference_test import fixture, save
from snapshot_publication_test import sample


def inventory(root):
    return ({str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in root.rglob("*") if p.is_file()},
            sorted(str(p.relative_to(root)) for p in root.rglob("*") if p.is_dir()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True)
    parser.add_argument("--parent", required=True)
    parser.add_argument("--suite", choices=("faults", "soak"), required=True)
    parser.add_argument("--capacity-scale", type=int, choices=(1, 2), default=2)
    parser.add_argument("--seconds", type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error("--seconds须在1..3600")
    root = Path(tempfile.mkdtemp(prefix="v3-fault-soak-", dir=args.parent))
    inputs, evidence = root / "inputs", root / "evidence"
    inputs.mkdir()
    evidence.mkdir()
    command = [args.native, str(inputs), args.suite, str(args.capacity_scale), str(args.seconds)]
    try:
        for name in ("valid", "alias", "incomplete", "sha", "semantic"):
            (inputs / name).mkdir()
        sample(inputs / "valid", 100, "old", 11)
        sample(inputs / "valid", 101, "new", 22, True)
        sample(inputs / "alias", 100, "alias", 33)
        for name in ("incomplete", "sha"):
            sample(inputs / name, 101, "new", 22, True)
        (inputs / "incomplete" / "101" / "COMPLETE").unlink()
        payload = inputs / "sha" / "101" / "documents.bin"
        raw = payload.read_bytes()
        payload.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
        # 全部SHA正确但词表语义非法，避免只验证外层校验和。
        invalid = fixture([(22, {"shared": 1})], {"shared-new": 3}, 1, 101)
        invalid["payloads"]["search-lexicon.txt"] = b"SIFTWING-LEXICON 1\nS 1\nT 0 ff\n"
        save(inputs / "semantic", invalid)
        before = inventory(inputs)
        (evidence / "input-baseline.json").write_text(json.dumps(before, indent=2))
        try:
            result = subprocess.run(command, capture_output=True, timeout=args.seconds + 100)
        except subprocess.TimeoutExpired as error:
            (evidence / "stdout").write_bytes(error.stdout or b"")
            (evidence / "stderr").write_bytes(error.stderr or b"")
            raise
        (evidence / "stdout").write_bytes(result.stdout)
        (evidence / "stderr").write_bytes(result.stderr)
        (evidence / "command.json").write_text(json.dumps({"argv": command, "exit_code": result.returncode}))
        print(result.stdout.decode(errors="replace"), end="", flush=True)
        if result.returncode or result.stderr:
            raise RuntimeError((result.returncode, result.stderr.decode(errors="replace")))
        summary = json.loads(result.stdout)
        if summary["suite"] != args.suite or summary["threads_after"] < 1:
            raise RuntimeError("缺少原生验收回执")
        if before != inventory(inputs):
            raise RuntimeError("测试修改了输入文件或目录集合")
    except BaseException:
        print(f"失败原件保留：{root}", flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
