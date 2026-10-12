"""从原始TF独立计算Decimal余弦与全局前k条，不读取生产完整排序作为参考。"""
import argparse
from collections import Counter
from decimal import Decimal, localcontext
import math
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
rng = random.Random(130)
maximum = (1 << 64) - 1
cases = []


def add(docs, words, mode, k, max_k, budget):
    tf = Counter(words)
    known = set(tf) & set().union(*(set(doc) for _, doc in docs))
    candidates = [(number, doc) for number, doc in docs
                  if (set(tf).issubset(doc) if mode == 0 else bool(set(tf) & set(doc)))]
    admitted = sum(len(set(tf) & set(doc)) for _, doc in docs)
    error = None
    if not max_k:
        error = 'top_k.limits'
    elif k > max_k:
        error = 'top_k.top_k'
    elif not all(budget):
        error = 'ranking.limits'
    elif len(tf) > budget[0]:
        error = 'candidate.terms'
    elif admitted > budget[1]:
        error = 'candidate.postings'
    elif len(candidates) > budget[2]:
        error = 'candidate.documents'
    elif len(candidates)*len(known) > budget[3]:
        error = 'ranking.work'
    expected = []
    if not error:
        with localcontext() as ctx:
            ctx.prec = 60
            vocabulary = set().union(*(set(doc) for _, doc in docs))
            idf = {t: Decimal(1) + (Decimal(len(docs)+1) /
                   Decimal(1+sum(t in doc for _, doc in docs))).ln() for t in vocabulary}
            qw = {t: Decimal(tf[t])*idf[t] for t in known}
            qnorm = sum((v*v for v in qw.values()), Decimal(0)).sqrt()
            for number, doc in candidates:
                dw = {t: Decimal(count)*idf[t] for t, count in doc.items()}
                dnorm = sum((v*v for v in dw.values()), Decimal(0)).sqrt()
                score = float(sum((qw[t]*dw.get(t, Decimal(0)) for t in known),
                                  Decimal(0))/(qnorm*dnorm))
                expected.append((number, score))
        expected.sort(key=lambda pair: (-pair[1], pair[0]))
        expected = expected[:k]
    fields = [mode, *budget, max_k, k, len(docs)]
    for number, doc in docs:
        fields += [number, len(doc)]
        for term, count in doc.items():
            fields += [term, count]
    fields += [len(words), *words]
    cases.append((' '.join(map(str, fields)), error, expected))


# 小整数生成非退化向量；加入完全相同文档，独立证明截断处精确同分的ID次序。
for iteration in range(80):
    ids = [0, maximum, 7, 9000000000000] + list(range(20, 50))
    rng.shuffle(ids)
    docs = [(ids[i], {t: rng.randrange(1, 32) for t in ['a', 'b', 'c', 'd']
                     if rng.randrange(3)}) for i in range(rng.randrange(20))]
    words = [rng.choice(['a', 'b', 'c', 'unknown']) for _ in range(rng.randrange(1, 8))]
    for mode in (0, 1):
        for k in sorted({1, 2, max(1, len(docs)), len(docs)+1, maximum}):
            add(docs, words, mode, k, maximum, (100, 1000, 100, 10000))
        for budget in [(0, 1000, 100, 10000), (1, 1000, 100, 10000),
                       (100, 1, 100, 10000), (100, 1000, 1, 10000), (100, 1000, 100, 1)]:
            add(docs, words, mode, 1, 1, budget)
        add(docs, words, mode, 1, 0, (0, 0, 0, 0))
        add(docs, words, mode, 2, 1, (0, 0, 0, 0))
for docs in [[], [(maximum, {'a': 3, 'b': 4}), (0, {'a': 3, 'b': 4})],
             [(maximum, {'a': maximum})], [(0, {}), (maximum, {})],
             [(maximum, {'a': 1}), (0, {'a': 7}), (7, {'a': 3})]]:
    for mode in (0, 1):
        for words in [['a'], ['a', 'unknown'], ['unknown'], ['a', 'a', 'b']]:
            for k in [1, 2, 3, maximum]:
                add(docs, words, mode, k, maximum, (100, 1000, 100, 10000))

payload = '\n'.join(line for line, _, _ in cases)+'\n'
previous = None
for repeat in range(2):
    result = subprocess.run([args.probe], input=payload, text=True, capture_output=True, timeout=45)
    if result.returncode:
        raise RuntimeError((result.returncode, result.stderr))
    lines = result.stdout.splitlines()
    if len(lines) != len(cases):
        raise AssertionError(('line count', len(lines), len(cases)))
    for i, (line, (_, error, expected)) in enumerate(zip(lines, cases)):
        if error:
            if line != 'ERR '+error:
                raise AssertionError((i, line, error))
            continue
        fields = line.split()
        if fields[:2] != ['OK', str(len(expected))] or len(fields) != 2+2*len(expected):
            raise AssertionError((i, line, expected))
        actual = [(int(fields[j]), float(fields[j+1])) for j in range(2, len(fields), 2)]
        if [n for n, _ in actual] != [n for n, _ in expected]:
            raise AssertionError(('global top-k identities', i, actual, expected))
        for (_, score), (_, wanted) in zip(actual, expected):
            if not math.isfinite(score) or not 0 < score <= 1 or not math.isclose(
                    score, wanted, rel_tol=3e-14, abs_tol=3e-15):
                raise AssertionError(('score', i, score, wanted))
        if actual != sorted(actual, key=lambda pair: (-pair[1], pair[0])):
            raise AssertionError(('exact published order', i, actual))
    if previous is not None and previous != result.stdout:
        raise AssertionError('fresh processes differ')
    previous = result.stdout
print(f'top-k independent cases={len(cases)} processes=2 failures=0')
