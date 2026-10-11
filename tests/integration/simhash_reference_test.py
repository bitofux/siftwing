"""固定字节hash与任意精度有符号投票参考；不读取生产源码，不使用其正票算法。"""
import argparse
from collections import Counter
import random
import subprocess


MASK = (1 << 64) - 1


def word_hash(word):
    value = 14695981039346656037
    for byte in word.encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) % (1 << 64)
    for shift, multiplier in [(33, 0xff51afd7ed558ccd), (33, 0xc4ceb9fe1a85ec53)]:
        value = ((value ^ (value >> shift)) * multiplier) % (1 << 64)
    return value ^ (value >> 33)


def reference(table):
    if not table:
        return None
    hashes = {word: word_hash(word) for word in table}
    votes = [sum(weight if (hashes[word] >> bit) & 1 else -weight
                 for word, weight in table.items()) for bit in range(64)]
    return sum(1 << bit for bit, vote in enumerate(votes) if vote > 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    rng = random.Random(115)
    words = ['a', 'b', 'red', 'blue42', '中国', '北京', '搜索', 'one', 'é', '😀', '𠀀', '#', "a's", '\ufeff']
    documents = [{}, {'a': 1}, {'中国': 1}, {'😀': 1}, {'a': 1, 'b': 1},
                 {'a': MASK}, {'a': MASK // 2 + 1, 'b': MASK // 2},
                 {'a': MASK // 2, 'b': MASK // 2}]
    for _ in range(100):
        table = Counter(rng.choice(words) for _ in range(rng.randrange(1, 300)))
        documents.extend([dict(table), {w: n * 1000000000000 for w, n in table.items()}])
    # 不同插入顺序、全合法标量类别、长词、超2^53的精确一票差。
    documents += [dict(reversed(list(documents[10].items()))), {'z' * 10000: 2},
                  {'a': (1 << 53) + 1, 'b': 1 << 53}]
    payload = str(len(documents)) + '\n'
    for table in documents:
        payload += f'{sum(table.values())} {len(table)}\n'
        payload += ''.join(f'{w.encode().hex()} {n}\n' for w, n in table.items())
    result = subprocess.run([args.probe], input=payload.encode(), capture_output=True, check=False)
    if result.returncode != 0:
        raise AssertionError((result.returncode, result.stdout, result.stderr))
    actual = [None if line == b'null' else int(line) for line in result.stdout.splitlines()]
    expected = [reference(table) for table in documents]
    if actual != expected:
        raise AssertionError([(i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b])
    # 固定数值约束hash，另验证符号投票关系，避免仅用一套参考互证。
    if word_hash('a') != 0x82a2a958a9bece5b or word_hash('中国') != 0xb76ab2bc7f31fcf1:
        raise AssertionError('fixed byte hash vectors')
    print(f'simhash independent reference cases={len(documents)} failures=0')


if __name__ == '__main__':
    main()
