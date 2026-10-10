#!/usr/bin/env python3
"""用独立Python字节参考覆盖全部Unicode标量及原创混合文本；不读私有输入。"""
import argparse
import json
import random
import re
import subprocess
import unittest
from pathlib import Path

PROBE = None
SPACES = '\u0009\u000a\u000b\u000c\u000d\u0020\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000'
CAPITALS = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')
def reference(text):
    # 字符属性显式固定；Python str.lower/str.split的范围更大，不能冒充本合同。
    return re.sub('[' + SPACES + ']+', ' ', text.translate(CAPITALS)).strip(' ')

class NormalizeReferenceTest(unittest.TestCase):
    def invoke(self, raw, maximum=16777216, output=16777216):
        return subprocess.run([str(PROBE), str(maximum), str(output)], input=raw,
                              capture_output=True, timeout=30)
    def normalized(self, raw, expected=None):
        result = self.invoke(raw)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        report = json.loads(result.stdout)
        self.assertEqual(report['policy_version'], 1)
        text = bytes.fromhex(report['text_hex'])
        self.assertEqual(text, reference(raw.decode()).encode() if expected is None else expected)
        return text
    def test_every_legal_unicode_scalar_except_rejected_nul(self):
        # 独立Python chr/UTF-8编码覆盖0x1..0x10ffff，跳过代理区；非字符和BOM也必须保留。
        raw = ''.join(chr(c) for c in range(1, 0x110000) if not 0xd800 <= c <= 0xdfff).encode()
        text = self.normalized(raw)
        self.normalized(text, text)
    def test_seeded_mixed_text_is_deterministic_and_idempotent(self):
        rng = random.Random(1102026)
        alphabet = list('ABCxyz123.,+-_/!?中文ÉΣİß😀\u0301\ufeff\u200b') + list(SPACES)
        for _ in range(30):
            raw = ''.join(rng.choices(alphabet, k=rng.randrange(0, 1000))).encode()
            text = self.normalized(raw)
            self.normalized(text, text)
    def test_late_bad_bytes_are_not_hidden_by_output_overflow(self):
        for raw, context in [(b'LONG\xff', 'normalize.utf8'), (b'LONG\0', 'normalize.nul')]:
            result = self.invoke(raw, output=1)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(result.stdout)['error_context'], context)
    def test_final_budget_and_probe_loading_are_separate(self):
        result = self.invoke('　A　'.encode(), maximum=7, output=1)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(bytes.fromhex(json.loads(result.stdout)['text_hex']), b'a')
        self.assertEqual(json.loads(self.invoke(b'ab', output=1).stdout)['error_context'], 'normalize.output_bytes')
        self.assertEqual(json.loads(self.invoke(b'ab', maximum=1).stdout)['error_context'], 'probe.input_bytes')
        self.assertEqual(self.invoke(b'', maximum=0).returncode, 2)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    PROBE = args.probe.resolve(strict=True)
    unittest.main(argv=['normalize_reference'], verbosity=2)
