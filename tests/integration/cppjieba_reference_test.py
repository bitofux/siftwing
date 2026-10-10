"""原创单字等频词典的独立参考：无歧义Han逐字，ASCII按正则成段，其他分隔。"""
import argparse
import json
import random
import re
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    # 只用已知单字，不调用cppjieba计算期望值。库算法黄金另由C++手写词典/HMM验证。
    with tempfile.TemporaryDirectory(prefix='siftwing-cppjieba-reference-') as directory:
        root = Path(directory)
        dictionary = root / 'dict'
        model = root / 'hmm'
        dictionary.write_text('中 1 n\n文 1 n\n国 1 n\n', encoding='utf-8')
        model.write_text(('0 -100 -100 -100\n' + '-100 0 -100 -100\n' * 4
                          + '中:0,文:0,国:0\n' * 4), encoding='utf-8')
        rng = random.Random(112)
        cases = ['', '中文A1国 A1中文', 'ABC_123，中文😀é', '\ufeff中文\u200b国']
        # 逐块端点与紧邻非汉字码点：显式合同参考，避免仅覆盖BMP。
        intervals = [(0x3400,0x4dbf),(0x4e00,0x9fff),(0xf900,0xfaff),
                     (0x20000,0x2a6df),(0x2a700,0x2b73f),(0x2b740,0x2b81f),
                     (0x2b820,0x2ceaf),(0x2ceb0,0x2ebef),(0x2ebf0,0x2ee5f),
                     (0x2f800,0x2fa1f),(0x30000,0x3134f),(0x31350,0x323af)]
        points = sorted({0x3007,0x3006,0x3008} | {p for a,b in intervals for p in (a-1,a,b,b+1)})
        boundary = '|'.join(map(chr, points))
        cases.append(boundary)
        cases += [''.join(rng.choice(['中', '文', '国', 'A', 'b', '0', '-', ' ', '😀', 'é', '\t', '\u0301'])
                          for _ in range(rng.randrange(1, 150))) for _ in range(80)]
        for text in cases:
            command = [args.probe, '10000', '1000', '1000', '10000', str(dictionary), str(model), '-', '10000', '0']
            result = subprocess.run(command, input=text.encode(), capture_output=True, check=False)
            if result.returncode:
                raise AssertionError((command, result.returncode, result.stdout, result.stderr))
            actual = [bytes.fromhex(item).decode() for item in json.loads(result.stdout)['tokens_hex']]
            expected = ([chr(p) for p in points if p == 0x3007 or any(a <= p <= b for a,b in intervals)]
                        if text == boundary else re.findall(r'[A-Za-z0-9]+|[中文国]', text))
            if actual != expected:
                raise AssertionError((text, actual, expected))
    print(f'cppjieba independent reference cases={len(cases)} failures=0')


if __name__ == '__main__':
    main()
