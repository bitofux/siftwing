"""从原始TF逐文档算60位Decimal余弦，不读取生产IDF/单位权重或复制Posting策略。"""
import argparse
from collections import Counter
from decimal import Decimal, localcontext
import math
import random
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
rng = random.Random(129)
maximum = (1 << 64) - 1
vocabulary = ['a', 'b', 'c', 'd', 'e', 'f', '17']
cases = []


def add_case(docs, words, mode, budgets):
    tf = Counter(re.findall(r'[a-z]+|[0-9]+', ' '.join(words).lower()))
    terms = set(tf)
    corpus_terms = set().union(*(set(doc) for _, doc in docs))
    known = terms & corpus_terms
    admitted = sum(len(terms & set(doc)) for _, doc in docs)
    candidates = [(number, doc) for number, doc in docs
                  if (terms.issubset(doc) if mode == 0 else bool(terms & set(doc)))]
    with localcontext() as ctx:
        ctx.prec = 60
        # 直接按全部原始文档统计N/DF，平方和开方归一化；独立于生产hypot路径。
        idf = {term: Decimal(1) + (Decimal(len(docs) + 1) /
               Decimal(1 + sum(term in doc for _, doc in docs))).ln() for term in corpus_terms}
        qw = {term: Decimal(tf[term]) * idf[term] for term in known}
        qnorm = sum((v*v for v in qw.values()), Decimal(0)).sqrt()
        scores = {}
        for number, doc in candidates:
            dw = {term: Decimal(count) * idf[term] for term, count in doc.items()}
            dnorm = sum((v*v for v in dw.values()), Decimal(0)).sqrt()
            scores[number] = float(sum((qw[t]*dw.get(t, Decimal(0)) for t in known),
                                      Decimal(0)) / (qnorm*dnorm))
    endpoint = (len(terms), max(1, admitted), max(1, len(candidates)),
                max(1, len(candidates)*len(known)))
    for budget in budgets(endpoint):
        context = None
        if not all(budget):
            context = 'ranking.limits'
        elif len(terms) > budget[0]:
            context = 'candidate.terms'
        elif admitted > budget[1]:
            context = 'candidate.postings'
        elif len(candidates) > budget[2]:
            context = 'candidate.documents'
        elif len(candidates)*len(known) > budget[3]:
            context = 'ranking.work'
        fields = [mode, *budget, rng.choice([1, 10, maximum]), len(docs)]
        for number, doc in docs:
            fields += [number, len(doc)]
            for word, count in doc.items():
                fields += [word, count]
        fields += [len(words), *words]
        cases.append((' '.join(map(str, fields)), context, scores))


def budgets(endpoint):
    result = [endpoint, (maximum,)*4, (0,)*4]
    for slot in range(4):
        short = list(endpoint)
        short[slot] = max(1, short[slot]-1)
        result.append(tuple(short))
    return result


for iteration in range(100):
    count = rng.randrange(25)
    pool = [0, maximum, 7, 99, 1000000000000] + list(range(100, 140))
    rng.shuffle(pool)
    docs = [(pool[i], {w: rng.randrange(1, 1000) for w in vocabulary if rng.randrange(3) == 0})
            for i in range(count)]
    words = [rng.choice(vocabulary + ['unknown', 'z']) for _ in range(rng.randrange(1, 15))]
    words = [w.upper() if rng.randrange(2) else w for w in words]
    for mode in (0, 1):
        add_case(docs, words, mode, budgets)
# 整数TF覆盖binary64精度边界与完整uint64范围；空篇计N，极稀疏身份不作下标。
for docs, words in [
    ([(maximum, {'a': maximum})], ['a']),
    ([(0, {'a': (1 << 53)+1, 'b': 7}), (maximum, {'b': maximum-(1 << 53)-8}), (99, {})], ['a', 'b', 'b']),
    ([(maximum, {'a': 3, 'b': 4})], ['a']*3+['b']*4),
    ([(maximum, {'a': 3, 'b': 4})], ['a', 'unknown', 'unknown']),
    ([(maximum, {'a': 3, 'b': 4}), (0, {'a': 3, 'b': 4})], ['a', 'b']),
]:
    for mode in (0, 1):
        add_case(docs, words, mode, budgets)

payload = '\n'.join(line for line, _, _ in cases) + '\n'
previous = None
for repeat in range(2):
    result = subprocess.run([args.probe], input=payload, text=True, capture_output=True, timeout=45)
    if result.returncode:
        raise RuntimeError((repeat, result.returncode, result.stderr))
    lines = result.stdout.splitlines()
    if len(lines) != len(cases):
        raise AssertionError(('line count', len(lines), len(cases)))
    for i, (line, (_, context, scores)) in enumerate(zip(lines, cases)):
        if context:
            if line != 'ERR '+context:
                raise AssertionError((i, line, context))
            continue
        fields = line.split()
        if fields[:2] != ['OK', str(len(scores))] or len(fields) != 2+2*len(scores):
            raise AssertionError((i, line, scores))
        actual = [(int(fields[j]), float(fields[j+1])) for j in range(2, len(fields), 2)]
        if len({n for n, _ in actual}) != len(actual) or {n for n, _ in actual} != set(scores):
            raise AssertionError(('identities', i, actual, scores))
        for number, score in actual:
            if not math.isfinite(score) or not 0 < score <= 1 or not math.isclose(
                    score, scores[number], rel_tol=3e-14, abs_tol=3e-15):
                raise AssertionError(('score', i, number, score, scores[number]))
        # 数学参考用容差仅核数值准确度；发布顺序必须按实际分数精确比较。
        if actual != sorted(actual, key=lambda pair: (-pair[1], pair[0])):
            raise AssertionError(('order', i, actual))
    if previous is not None and result.stdout != previous:
        raise AssertionError('fresh processes differ byte-for-byte')
    previous = result.stdout
print(f'ranking independent cases={len(cases)} processes=2 failures=0')
