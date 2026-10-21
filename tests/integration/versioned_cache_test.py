#!/usr/bin/env python3
"""用公开原创黄金和不同版本正文驱动真实缓存服务；失败现场保留。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import cli_test as reference


def main():
    parser = argparse.ArgumentParser()
    for name in ["indexer", "writer", "native"]:
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--capacity-scale", choices=["1", "2"], required=True)
    options = parser.parse_args()
    reference.OPTIONS = options
    root = Path(tempfile.mkdtemp(prefix="siftwing-versioned-cache-"))
    try:
        parent, _, _ = reference.golden(root)
        changed = reference.write(root, "changed.txt", "red changed body")
        second = reference.build(root, [("txt", changed)], version=2**64-1)
        result = subprocess.run([options.native, str(parent), str(second), options.capacity_scale],
                                capture_output=True, text=True, timeout=30)
        print(result.stdout, end="")
        print(result.stderr, end="")
        if result.returncode:
            raise RuntimeError(f"native exit={result.returncode}")
    except BaseException:
        print(f"失败夹具保留于：{root}")
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
