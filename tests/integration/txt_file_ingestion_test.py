#!/usr/bin/env python3
"""用临时原创文件验证TXT接入；不依赖私有语料或生产加载API。"""

import argparse
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import unittest


PROBE = None


def decoded(item, field):
    return bytes.fromhex(item["record"][field + "_hex"])


class TxtFileIngestionTest(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix="siftwing-txt-original-")
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name) / "inputs"
        self.root.mkdir()

    def file(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def invoke(self, paths, limits=(8 * 1024 * 1024, 8 * 1024 * 1024, 128, 16 * 1024 * 1024),
               root=None, fd_limit=None):
        def restrict_fds():
            resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))
        return subprocess.run([str(PROBE), str(root or self.root), *map(str, limits), *paths],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
                              preexec_fn=restrict_fds if fd_limit is not None else None)

    def report(self, paths, **kwargs):
        result = self.invoke(paths, **kwargs)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        report = json.loads(result.stdout)
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(sum(report["counts"].values()), len(report["items"]))
        self.assertEqual(report["total_text_bytes"], sum(
            len(decoded(item, "title")) + len(decoded(item, "content"))
            for item in report["items"] if item["record"] is not None))
        return report

    def test_complete_files_are_sorted_owned_and_preserved(self):
        original = "作者：原创\r\n正文：山河\r第1章 42!\n正文：文中标记。".encode()
        self.file("notes/甲.txt", b"\xef\xbb\xbf" + original)
        self.file("english.txt", b"Chapter 1\r\nApple, apple 42!\r\n")
        expected = "作者：原创\n正文：山河\n第1章 42!\n正文：文中标记。".encode()
        report = self.report(["notes/甲.txt", "english.txt"])
        self.assertEqual(report, self.report(["english.txt", "notes/甲.txt"]))
        self.assertEqual(report["counts"]["warning"], 2)
        self.assertIsNone(report["stop"])
        first, second = report["items"]
        self.assertEqual([first["record"]["id"], second["record"]["id"]], [0, 1])
        self.assertEqual([first["ordinal"], second["ordinal"]], [0, 0])
        self.assertEqual(decoded(first, "content"), b"Chapter 1\nApple, apple 42!\n")
        self.assertEqual(decoded(second, "title"), "甲".encode())
        self.assertEqual(decoded(second, "content"), expected)
        self.assertEqual((self.root / "notes/甲.txt").read_bytes(), b"\xef\xbb\xbf" + original)

    def test_empty_bad_encoding_nul_and_io_failure_continue(self):
        self.file("a.txt", " \t\r\n\u3000".encode())
        self.file("b.txt", b"x\xff")
        self.file("c.txt", b"x\0y")
        self.file("z.txt", b"intact")
        report = self.report(["z.txt", "missing.txt", "c.txt", "b.txt", "a.txt"])
        self.assertEqual([item["status"] for item in report["items"]],
                         ["no_text", "rejected", "rejected", "failed", "warning"])
        self.assertTrue(all(item["record"] is None for item in report["items"][:-1]))
        self.assertEqual(decoded(report["items"][-1], "content"), b"intact")
        self.assertIsNone(report["stop"])

    def test_input_and_document_bounds_are_separate_inclusive(self):
        self.file("a.txt", b"\xef\xbb\xbf" + "甲\r\n乙".encode())
        exact = self.report(["a.txt"], limits=(11, 8, 1, 8))
        self.assertEqual(exact["counts"]["warning"], 1)
        self.assertEqual(decoded(exact["items"][0], "content"), "甲\n乙".encode())
        for limits, code in [((10, 8, 1, 8), 3), ((11, 7, 1, 8), 4)]:
            report = self.report(["a.txt"], limits=limits)
            self.assertEqual(report["counts"]["rejected"], 1)
            self.assertIsNone(report["items"][0]["record"])
            self.assertEqual(report["items"][0]["issues"][0]["code"], code)

    def test_long_file_keeps_one_document_and_rejects_whole(self):
        original = b"Chapter\r\nApple 42!\r\n" * 100000
        self.file("long.txt", original)
        expected = b"Chapter\nApple 42!\n" * 100000
        report = self.report(["long.txt"])
        self.assertEqual(len(report["items"]), 1)
        self.assertEqual(decoded(report["items"][0], "content"), expected)
        rejected = self.report(["long.txt"], limits=(len(original), len(expected), 1, len(expected) + 4))
        self.assertEqual(rejected["counts"]["rejected"], 1)  # 标题long的四字节也在预算内。
        self.assertIsNone(rejected["items"][0]["record"])

    def test_large_sparse_file_rejected_before_buffer_allocation(self):
        with (self.root / "large.txt").open("wb") as stream:
            stream.truncate(2 * 1024 * 1024 * 1024)
        report = self.report(["large.txt"], limits=(1024, 1024, 1, 1024))
        self.assertEqual(report["counts"]["rejected"], 1)
        self.assertEqual(report["items"][0]["issues"][0]["code"], 3)

    def test_batch_stop_does_not_publish_candidate(self):
        self.file("a.txt", b"abc")
        self.file("b.txt", b"def")
        limited = self.report(["b.txt", "a.txt"], limits=(3, 4, 1, 8))
        self.assertEqual(limited["stop"]["reason"], 1)
        self.assertEqual(len(limited["items"]), 1)
        full = self.report(["b.txt", "a.txt"], limits=(3, 4, 2, 7))
        self.assertEqual(full["stop"]["reason"], 2)
        self.assertEqual(len(full["items"]), 1)
        self.assertEqual(full["total_text_bytes"], 4)

    def test_symlinks_directories_fifo_and_unreadable_file_fail(self):
        outside = Path(self.workspace.name) / "outside.txt"
        outside.write_bytes(b"outside must not be imported")
        (self.root / "link.txt").symlink_to(outside)
        (self.root / "linked-dir").symlink_to(outside.parent, target_is_directory=True)
        (self.root / "dir").mkdir()
        os.mkfifo(self.root / "fifo")
        self.file("z.txt", b"valid")
        report = self.report(["link.txt", "linked-dir/outside.txt", "dir", "fifo", "z.txt"])
        self.assertEqual(report["counts"]["failed"], 4)
        self.assertEqual(report["counts"]["warning"], 1)
        self.assertTrue(all(item["record"] is None for item in report["items"][:-1]))
        inaccessible = self.file("locked.txt", b"no read permission")
        inaccessible.chmod(0)
        try:
            self.assertNotEqual(os.geteuid(), 0, "本测试要求普通用户运行")
            self.assertEqual(self.report(["locked.txt"])["counts"]["failed"], 1)
        finally:
            inaccessible.chmod(0o600)

    def test_file_descriptors_released_between_inputs(self):
        paths = ["file{:03d}.txt".format(index) for index in range(80)]
        for name in paths:
            self.file(name, b"original")
        report = self.report(paths, fd_limit=32)
        self.assertEqual(report["counts"]["warning"], len(paths))
        self.assertEqual(report["counts"]["failed"], 0)

    def test_invalid_cli_and_linked_root_produce_no_report(self):
        self.file("a.txt", b"safe")
        link = Path(self.workspace.name) / "root-link"
        link.symlink_to(self.root, target_is_directory=True)
        for paths in [[], ["../outside.txt"], ["/a.txt"], ["a/./b"], ["a//b"], ["a/"], ["a.txt", "a.txt"]]:
            result = self.invoke(paths)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b"")
        for value in ["0", "-1", "1x", "18446744073709551616"]:
            result = self.invoke(["a.txt"], limits=(value, 1024, 1, 1024))
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b"")
        self.assertEqual(self.invoke(["a.txt"], root=link).returncode, 2)

    def test_output_failure_has_nonzero_exit(self):
        self.file("a.txt", b"original")
        with open("/dev/full", "wb") as sink:
            result = subprocess.run([str(PROBE), str(self.root), "1024", "1024", "1", "1024", "a.txt"],
                                    stdout=sink, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    PROBE = args.probe.resolve(strict=True)
    unittest.main(argv=["txt_file_ingestion_test"], verbosity=2)
