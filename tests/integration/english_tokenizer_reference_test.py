"""用Python严格解码及独立ASCII正则验证英文规则，不依赖私有输入。"""

import argparse
import json
import random
import re
import subprocess
import unittest


PROBE = None


def reference(data):
    text = data.decode("utf-8", errors="strict")
    if "\0" in text:
        raise ValueError("NUL is forbidden")
    return [word.encode("ascii").hex() for word in re.findall(r"[A-Za-z0-9]+", text)]


def invoke(data, limits=None):
    if limits is None:
        size = max(1, len(data))
        limits = (size, size, size, size)
    completed = subprocess.run([PROBE] + [str(n) for n in limits], input=data,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    return completed, json.loads(completed.stdout)


class EnglishTokenizerReference(unittest.TestCase):
    def assert_reference(self, data):
        result, report = invoke(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report["policy_version"], 1)
        self.assertEqual(report["tokens_hex"], reference(data))
        # 拥有序列重复/顺序也参与相等，绝不只比较set或token数。
        return report["tokens_hex"]

    def test_unicode_scalar_boundary(self):
        # 覆盖全部合法非NUL Unicode标量；Python正则不使用\w的Unicode语义。
        text = "".join(chr(n) for n in range(1, 0x110000) if not 0xD800 <= n <= 0xDFFF)
        self.assert_reference(text.encode("utf-8"))

    def test_original_and_seeded_sequences(self):
        original = [b"", b"word", b"One ONE one 42 a1", b"don't e-mail foo_bar C++ 3.14",
                    "café résumé 中文A😀B a\u0301b Ａ１２".encode("utf-8")]
        for data in original:
            self.assert_reference(data)
        rng = random.Random(11120261010)
        alphabet = "aAZz019 ,;_-.!\t\r\n'" + "éΣ中文😀\u0301\ufeff\u200b\u00a0"
        for _ in range(40):
            self.assert_reference("".join(rng.choice(alphabet) for _ in range(2000)).encode("utf-8"))
        # 重复拼接有明确分隔，输出必须保持两份同序词，排除去重/排序实现。
        data = b"b2 a1 b2 z9"
        self.assertEqual(self.assert_reference(data + b";" + data), reference(data) * 2)

    def test_invalid_encoding_precedes_output_limits(self):
        for invalid in (b"\xff", b"\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe2\x82"):
            data = b"toolong " + invalid
            with self.assertRaises(UnicodeDecodeError):
                data.decode("utf-8", errors="strict")
            result, report = invoke(data, (len(data), 1, 1, 1))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(report["error_context"], "tokenizer.utf8")
            self.assertNotIn("tokens_hex", report)
        result, report = invoke(b"toolong\0", (8, 1, 1, 1))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report["error_context"], "tokenizer.nul")

    def test_exact_and_failed_limits(self):
        data = b"Ab 12"
        result, report = invoke(data, (5, 2, 2, 4))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(report["tokens_hex"], reference(data))
        for limits, context in [((5, 1, 2, 4), "tokens"), ((5, 2, 1, 4), "token_bytes"),
                                ((5, 2, 2, 3), "output_bytes")]:
            result, report = invoke(data, limits)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(report["error_context"], "tokenizer." + context)
            self.assertNotIn("tokens_hex", report)
        result, report = invoke(data, (4, 2, 2, 4))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report["error_context"], "probe.input_bytes")
        for limits in ((0, 1, 1, 1), (1, 0, 1, 1), (1, 1, 0, 1), (1, 1, 1, 0)):
            process = subprocess.run([PROBE] + [str(n) for n in limits], input=b"",
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(process.returncode, 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    arguments, remainder = parser.parse_known_args()
    PROBE = arguments.probe
    unittest.main(argv=[__file__] + remainder)
