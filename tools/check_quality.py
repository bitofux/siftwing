#!/usr/bin/env python3
"""只读质量门禁：保留精确的历史格式债务，阻断新增诊断或工具失败。"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("include", "src", "apps", "tests", "tools")
SUFFIXES = {".cc", ".cpp", ".h", ".hpp"}
# 有限范围覆盖fd、信号、队列、线程和快照生命周期；不冒称全项目静态分析。
TIDY_FILES = (
    "src/net/unique_fd.cc",
    "src/net/signalfd_source.cc",
    "src/net/eventfd_wakeup.cc",
    "src/net/completion_wakeup.cc",
    "src/concurrency/worker_pool.cc",
    "src/concurrency/completion_queue.cc",
    "src/snapshot/manager.cc",
    "src/snapshot/loader.cc",
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require_llvm18(tool):
    result = subprocess.run([tool, "--version"], check=True, capture_output=True, text=True)
    if not re.search(r"(?:version|LLVM version) 18\.", result.stdout):
        raise ValueError(f"需要LLVM 18工具：{tool}\n{result.stdout}")


def check_format(tool):
    require_llvm18(tool)
    baseline = json.loads((ROOT / "tools/format_baseline.json").read_text())
    if baseline["schema_version"] != 1 or baseline["clang_format_major"] != 18:
        raise ValueError("未知格式基线版本")
    if sha256((ROOT / ".clang-format").read_bytes()) != baseline["style_sha256"]:
        raise ValueError("格式配置变化，需要先审查历史债务的迁移")
    files = sorted(
        path for name in SOURCE_ROOTS for path in (ROOT / name).rglob("*")
        if path.is_file() and path.suffix in SUFFIXES
    )
    names = {str(path.relative_to(ROOT)) for path in files}
    if not names or set(baseline["files"]) - names:
        raise ValueError("源码集合为空，或历史基线指向不存在的文件")
    legacy = 0
    failures = []
    for path in files:
        # 不使用-i；工具失败不能被当作格式一致，也不修改活动源码。
        result = subprocess.run([tool, "--style=file", str(path)], check=True, capture_output=True)
        raw = path.read_bytes()
        if result.stdout != raw:
            name = str(path.relative_to(ROOT))
            if baseline["files"].get(name) == sha256(raw):
                legacy += 1
            else:
                failures.append(name)
    print(f"format: 检查{len(files)}文件，保留{legacy}个字节未变的历史违规", flush=True)
    if failures:
        raise ValueError("新增或修改文件未符合clang-format18：\n" + "\n".join(failures))


def check_tidy(tool, build_dir):
    require_llvm18(tool)
    build = build_dir.resolve(strict=True)
    database = json.loads((build / "compile_commands.json").read_text())
    files = set()
    for entry in database:
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        files.add(path.resolve())
    expected = {ROOT / name for name in TIDY_FILES}
    if not expected <= files:
        raise ValueError("编译数据库缺少指定生产翻译单元；不能跳过或使用其他源码树")
    header_filter = "^" + re.escape(str(ROOT)) + r"/(include|src|apps|tools)/"
    for name in TIDY_FILES:
        print(f"tidy: {name}", flush=True)
        subprocess.run(
            [tool, "-p", str(build), "--warnings-as-errors=*",
             "--header-filter=" + header_filter, str(ROOT / name)],
            cwd=ROOT, check=True,
        )
    print(f"tidy: {len(TIDY_FILES)}个生产翻译单元通过", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("format", "tidy"))
    parser.add_argument("--tool", help="覆盖工具路径，仍须为LLVM18")
    parser.add_argument("--build-dir", type=Path, help="tidy所需的同源码树CMake编译数据库目录")
    args = parser.parse_args()
    if args.mode == "tidy" and args.build_dir is None:
        parser.error("tidy需要--build-dir")
    try:
        if args.mode == "format":
            check_format(args.tool or "clang-format-18")
        else:
            check_tidy(args.tool or "clang-tidy-18", args.build_dir)
    except subprocess.CalledProcessError as error:
        # capture_output路径也要保留原始诊断；tidy继承输出时这两项为空。
        for value, stream in ((error.stdout, sys.stdout), (error.stderr, sys.stderr)):
            if value:
                stream.write(value.decode(errors="replace") if isinstance(value, bytes) else value)
        print(f"质量门禁失败：{error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"质量门禁失败：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
