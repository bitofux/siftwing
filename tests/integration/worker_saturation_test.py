#!/usr/bin/env python3
"""用原创公开夹具建真实快照；保留失败现场，不读取私有语料。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

import cli_test as reference


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--indexer", required=True)
    parser.add_argument("--writer", required=True)
    parser.add_argument("--native", required=True)
    parser.add_argument("--subs", choices=["1", "2"], required=True)
    options = parser.parse_args()
    reference.OPTIONS = options
    root = Path(tempfile.mkdtemp(prefix="siftwing-worker-saturation-"))
    try:
        parent, _, _ = reference.golden(root)
        result = subprocess.run([options.native, str(parent), options.subs],
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
