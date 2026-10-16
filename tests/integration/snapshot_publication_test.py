#!/usr/bin/env python3
"""原创可查询八文件：独立记录编码、版本端点、回滚与同版本异内容。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from reader_reference_test import fixture, save, record, table, sha


def sample(parent, version, label, identity, extra=False):
    docs = [(identity, {"shared": 1})]
    if extra:
        docs.append((identity + 1, {"other": 1}))
    f = fixture(docs, {"shared-" + label: 3}, 1, version)
    library, entries = b"", []
    for doc_id, _ in docs:
        encoded = record(doc_id, 0, 1, [label.encode(), ("shared " + label).encode(),
                                       label.encode(), None, None, None])
        entries.append((doc_id, len(library), len(encoded)))
        library += encoded
    f["payloads"]["documents.bin"] = library
    f["payloads"]["offsets.bin"] = table(entries, len(library))
    leaf = save(parent, f)
    meta = (leaf / "manifest.txt").read_bytes().replace(b"independent-v1", label.encode())
    (leaf / "manifest.txt").write_bytes(meta)
    (leaf / "COMPLETE").write_text(f"SIFTWING-COMPLETE 1 {version} {sha(meta)}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--parent", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--signals", action="store_true")
    mode.add_argument("--races", action="store_true")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="snapshot-publication-", dir=args.parent))
    try:
        valid, alias, broken = (root / name for name in ("valid", "alias", "broken"))
        for p in (valid, alias, broken):
            p.mkdir()
        sample(valid, 100, "old", 11)
        sample(valid, 101, "new", 22, True)
        sample(valid, 0, "zero", 44)
        sample(valid, (1 << 64) - 1, "maximum", 55)
        save(valid, fixture([], {}, 0, 2))
        sample(alias, 100, "alias", 33)
        sample(broken, 101, "bad", 66)
        (broken / "101" / "COMPLETE").unlink()
        before = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob("*") if p.is_file()}
        proc = subprocess.run([args.native, str(valid), str(alias), str(broken), str(root / "events"), *(["races"] if args.races else ["signals"] if args.signals else [])],
                              capture_output=True, text=True, timeout=110)
        print(proc.stdout, end="", flush=True)
        if proc.returncode:
            raise RuntimeError((proc.returncode, proc.stderr))
        after = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob("*")
                 if p.is_file() and p.name != "events"}
        if before != after:
            raise RuntimeError("发布改写了输入或留下额外文件")
    except BaseException:
        print(f"失败原件保留：{root}", flush=True)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
