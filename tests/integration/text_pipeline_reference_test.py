"""原创独立ASCII/Unicode参考与可手算中文黄金，校验完整序列和TF。"""
import argparse
from collections import Counter
import json
import random
import re
import subprocess

SPACES = '\u0009\u000a\u000b\u000c\u000d\u0020\u0085\u00a0\u1680' + ''.join(
    chr(i) for i in range(0x2000, 0x200B)) + '\u2028\u2029\u202f\u205f\u3000'
LOWER = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def english(text):
    normalized = re.sub('[' + SPACES + ']+', ' ', text.translate(LOWER)).strip(' ')
    return [word for word in re.findall('[a-z0-9]+', normalized) if word not in {'the', 'and', '中文'}]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    rng = random.Random(114)
    cases = [('E', text, english(text)) for text in (
        '', ' THE and THE ', 'ONE one\r\nBlue42', "Café foo_bar DON'T 3.14 中文",
        ''.join(space + 'A1' for space in SPACES), '\ufeffTHE\ufeffKEEP')]
    alphabet = list('ABCxyz012_\'-') + list(SPACES) + ['中文', '😀', 'É', '\u0301', '\ufeff']
    for _ in range(40):
        text = ''.join(rng.choice(alphabet) for _ in range(300))
        cases.append(('E', text, english(text)))
    # 英文路径对全部合法非NUL Unicode标量的分类独立参考；不以生产helper生成预期。
    scalars = ''.join(chr(i) for i in range(1, 0x110000) if not 0xD800 <= i <= 0xDFFF)
    cases.append(('E', scalars, english(scalars)))
    # 原创词典无HMM：四个已知词固定，未知汉字按单字；期望由手算给出。
    cases.extend([
        ('C', '中国北京 中文　ABC abc', ['中国', '北京', 'abc', 'abc']),
        ('C', 'THE 曙光 中国 中国 and', ['曙', '光', '中国', '中国']),
        ('C', '中文中文', []),
        ('C', '北京 123 123！', ['北京', '123', '123']),
    ])
    payload = ''.join(mode + text.encode().hex() + '\n' for mode, text, _ in cases)
    completed = subprocess.run([args.probe, '--fixture'], input=payload, text=True,
                               capture_output=True, timeout=90, check=True)
    report = json.loads(completed.stdout)
    if len(report['documents']) != len(cases):
        raise AssertionError('document boundaries changed')
    for (_, _, expected), actual in zip(cases, report['documents']):
        words = [bytes.fromhex(word).decode() for word in actual['tokens_hex']]
        counts = Counter(expected)
        terms = [[word.encode().hex(), counts[word]] for word in sorted(counts, key=str.encode)]
        if words != expected or actual['terms'] != terms or actual['total_tokens'] != len(expected):
            raise AssertionError('independent ordered tokens/TF mismatch')
    print(f'text pipeline independent cases={len(cases)} full_non_nul_scalars={len(scalars)}')


if __name__ == '__main__':
    main()
