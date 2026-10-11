"""逐字符64位字符串距离与独立保留列表参考，检验完整审计和限额失败；不读取生产源码。"""
import argparse
import random
import subprocess


MAX = (1 << 64) - 1


def distance(lhs, rhs):
    return sum(a != b for a, b in zip(f'{lhs:064b}', f'{rhs:064b}'))


def reference(documents, threshold, budget):
    kept, decisions = [], []
    comparisons, duplicates, empty = 0, 0, 0
    for identity, path, ordinal, bits in sorted(documents, key=lambda d: (d[1].encode(), d[2])):
        match = None
        if bits is None:
            empty += 1
            status = 'no_features'
        else:
            for prior_id, prior_bits in kept:
                if comparisons >= budget:
                    return ['error duplicate.comparisons']
                comparisons += 1
                difference = distance(prior_bits, bits)
                if difference <= threshold:
                    match = (prior_id, difference)
                    break
            if match is None:
                kept.append((identity, bits))
                status = 'kept'
            else:
                status = 'duplicate'
                duplicates += 1
        suffix = '- -' if match is None else f'{match[0]} {match[1]}'
        decisions.append(f'{identity} {path} {ordinal} {status} {suffix}')
    return [f'ok {len(documents)} {len(kept)} {duplicates} {empty} {comparisons}', *decisions]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    rng = random.Random(116)
    cases = [([], 3, 1), ([(0, 'a', 0, None), (MAX, 'b', MAX, None)], 64, 1)]
    for threshold in range(65):
        cases.append(([(9, 'a', 0, 0), (0, 'b', 0, (1 << threshold) - 1)], threshold, 1))
    for _ in range(100):
        count = rng.randrange(1, 45)
        centers = [rng.getrandbits(64) for _ in range(4)]
        ids = rng.sample(range(1000), count)
        documents = []
        for index in range(count):
            bits = rng.choice(centers)
            for bit in rng.sample(range(64), rng.randrange(7)):
                bits ^= 1 << bit
            if rng.randrange(8) == 0:
                bits = None
            documents.append((ids[index], ['z', 'a', '中'][index % 3], index // 3, bits))
        threshold = rng.choice([0, 1, 3, 6, 32, 63, 64])
        budget = rng.choice([1, 2, 10, 1000, MAX])
        cases.append((documents, threshold, budget))
        shuffled = documents.copy()
        rng.shuffle(shuffled)
        cases.append((shuffled, threshold, budget))
    # 丢弃者传递聚类和选最近者都与此固定参考不同；小预算只计实际比较。
    cases += [([(8, 'a', 0, 0), (2, 'b', 0, 7), (0, 'c', 0, 63)], 3, 2),
              ([(8, 'a', 0, 0), (2, 'b', 0, 15), (0, 'c', 0, 7)], 3, 2)]
    payload = f'{len(cases)}\n'
    expected = []
    for documents, threshold, budget in cases:
        payload += f'{len(documents)} {threshold} {budget}\n'
        for identity, path, ordinal, bits in documents:
            payload += f'{identity} {path} {ordinal} {int(bits is not None)} {bits or 0}\n'
        expected.extend(reference(documents, threshold, budget))
    result = subprocess.run([args.probe], input=payload, text=True, capture_output=True, check=False)
    if result.returncode != 0 or result.stdout.splitlines() != expected:
        raise AssertionError((result.returncode, result.stderr,
                              next(((i, a, b) for i, (a, b) in enumerate(zip(result.stdout.splitlines(), expected))
                                    if a != b), 'line count or probe failure')))
    print(f'duplicate independent reference cases={len(cases)} failures=0')


if __name__ == '__main__':
    main()
