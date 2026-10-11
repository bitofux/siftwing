"""从原始token及Python任意精度整数计算参考；不读生产词典来生成预期。"""
import argparse
from collections import Counter
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
rng = random.Random(121)
checks = 0

def verify(documents):
    global checks
    counts = {doc: Counter(tokens) for doc, tokens in documents}
    terms = sorted({term for _, tokens in documents for term in tokens}, key=lambda t: t.encode('utf-8'))
    relations = sum(len(c) for c in counts.values())
    expected = [f'S {len(documents)} {sum(len(t) for _, t in documents)} {relations}']
    for doc, tokens in sorted(documents):
        expected.append(f'D {doc} {len(tokens)} {len(counts[doc])}')
    for term_id, term in enumerate(terms):
        present = [(doc, counts[doc][term]) for doc in sorted(counts) if term in counts[doc]]
        expected.append(f'T {term_id} {term.encode("utf-8").hex()} {len(present)}')
        expected.extend(f'P {term_id} {doc} {tf}' for doc, tf in present)
    data = ''.join(str(doc) + ''.join(' ' + t.encode('utf-8').hex() for t in tokens) + '\n' for doc, tokens in documents)
    result = subprocess.run([args.probe], input=data, text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert actual == expected, (documents, expected, actual, result.stderr)
    checks += len(expected) + 1

verify([])
verify([(0, []), (2**64-1, [])])
verify([(42, ['apple']*3+['pear']), (2**64-1, []), (7, ['apple']*2+['banana']), (0, ['banana'])])
vocabulary = ['a', 'A', 'banana', '中文', '🙂', 'é', 'e\u0301', '\ufeff', '!']
for _ in range(80):
    ids = rng.sample([0, 1, 7, 42, 1000, 2**64-1], rng.randrange(7))
    documents = [(doc, [rng.choice(vocabulary) for _ in range(rng.randrange(31))]) for doc in ids]
    verify(documents)
    rng.shuffle(documents)
    verify(documents)
print(f'checks={checks} failures=0; 163 independent original-token batches')
