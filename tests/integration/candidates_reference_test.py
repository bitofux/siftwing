"""逐文档检查含词关系的独立参考；不调用生产合并算法或复制堆/二分策略。"""
import argparse
import random
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
rng = random.Random(128)
maximum = (1 << 64) - 1
vocabulary = ['a', 'b', 'c', 'd', 'e', 'f', '17']
cases = []
for iteration in range(100):
    count = rng.randrange(25)
    pool = [0, maximum, 7, 99, 1000000000000] + list(range(100, 140))
    rng.shuffle(pool)
    docs = [(pool[i], {word: rng.randrange(1, 100) for word in vocabulary
                     if rng.randrange(3) == 0}) for i in range(count)]
    words = [rng.choice(vocabulary + ['unknown', 'z']) for _ in range(rng.randrange(1, 12))]
    # 实际Query执行ASCII小写；大小写和重复不增加唯一布尔约束。
    words = [word.upper() if rng.randrange(2) else word for word in words]
    terms = set(re.findall(r'[a-z]+|[0-9]+', ' '.join(words).lower()))
    postings = sum(len(terms.intersection(tf)) for _, tf in docs)
    for mode in [0, 1]:
        gold = sorted(number for number, tf in docs
                      if (terms.issubset(tf) if mode == 0 else bool(terms.intersection(tf))))
        endpoint = (len(terms), max(1, postings), max(1, len(gold)))
        budgets = [endpoint, (maximum, maximum, maximum), (0, 0, 0),
                   (max(1, len(terms) - 1), endpoint[1], endpoint[2]),
                   (endpoint[0], max(1, postings - 1), endpoint[2]),
                   (endpoint[0], endpoint[1], max(1, len(gold) - 1))]
        for budget in budgets:
            context = None
            if not all(budget):
                context = 'candidate.limits'
            elif len(terms) > budget[0]:
                context = 'candidate.terms'
            elif postings > budget[1]:
                context = 'candidate.postings'
            elif len(gold) > budget[2]:
                context = 'candidate.documents'
            expected = 'ERR ' + context if context else 'OK ' + ' '.join(map(str, [len(gold)] + gold))
            fields = [mode, *budget, rng.choice([1, 10, maximum]), len(docs)]
            for number, tf in docs:
                fields += [number, len(tf)]
                for word, frequency in tf.items():
                    fields += [word, frequency]
            fields += [len(words), *words]
            cases.append((' '.join(map(str, fields)), expected))
payload = '\n'.join(line for line, _ in cases) + '\n'
expected = [gold for _, gold in cases]
for repeat in range(2):
    result = subprocess.run([args.probe], input=payload, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError((repeat, result.returncode, result.stderr))
    actual = result.stdout.splitlines()
    if actual != expected:
        mismatch = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]),
                        min(len(actual), len(expected)))
        raise AssertionError((repeat, mismatch, actual[mismatch:mismatch+1], expected[mismatch:mismatch+1]))
print(f'candidate independent cases={len(cases)} processes=2 failures=0')
