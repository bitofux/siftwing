#!/usr/bin/env python3
"""独立构造八文件输入：版本端点、空快照、损坏SHA及重签后的语义错误。"""
import argparse
import copy
from pathlib import Path
import shutil
import subprocess
import tempfile

from reader_reference_test import fixture, save, manifest, sha


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--parent", required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="snapshot-manager-", dir=args.parent))
    try:
        parent = root / "valid"
        parent.mkdir()
        sample = fixture([(0, {"red": 2}), (77, {"blue": 1})], {"blue": 1}, 2)
        save(parent, sample)
        save(parent, fixture([(0, {"red": 2}), (77, {"blue": 1})], {"blue": 1}, 2, (1 << 64) - 1))
        save(parent, fixture([], {}, 0, 2))
        # 所有反例仍经Manager真实Reader校验，不让管理层绕过格式或完整性门禁。
        for mode in ("sha", "semantic", "incomplete"):
            bad = root / mode
            bad.mkdir()
            invalid = copy.deepcopy(sample)
            invalid["version"] = 1
            leaf = save(bad, invalid)
            if mode == "sha":
                (leaf / "documents.bin").write_bytes(b"corrupt")
            elif mode == "semantic":
                invalid["payloads"]["search-lexicon.txt"] = b"SIFTWING-LEXICON 1\nS 1\nT 0 ff\n"
                (leaf / "search-lexicon.txt").write_bytes(invalid["payloads"]["search-lexicon.txt"])
                meta = manifest(invalid)
                (leaf / "manifest.txt").write_bytes(meta)
                (leaf / "COMPLETE").write_text(f"SIFTWING-COMPLETE 1 1 {sha(meta)}\n")
            else:
                (leaf / "COMPLETE").unlink()
            before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            result = subprocess.run([args.native, str(parent), str(bad)], capture_output=True, text=True, timeout=45)
            if result.returncode:
                raise RuntimeError((mode, result.returncode, result.stdout, result.stderr))
            after = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            if before != after:
                raise RuntimeError("管理器改写了输入")
            print(mode, result.stdout.strip())
    except BaseException:
        print(f"失败原件保留：{root}", flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
