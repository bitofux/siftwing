"""Python任意精度计数/80位Decimal独立参考；全字段核对并验证规范字节与排列确定性。"""
import argparse
from decimal import Decimal, localcontext
import hashlib
import math
import random
import re
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
args = parser.parse_args()
rng = random.Random(122)
checks = 0
transcripts = hashlib.sha256()

def check(condition, details):
    global checks
    checks += 1
    assert condition, details

def number(bits, expected):
    check(re.fullmatch('[0-9a-f]{16}', bits) is not None, bits)
    actual = struct.unpack('>d', bytes.fromhex(bits))[0]
    check(math.isfinite(actual) and math.isclose(actual, float(expected), rel_tol=1e-12, abs_tol=0.0), (actual, expected))
    if expected == 0:
        check(bits == '0000000000000000', bits)
    return actual

def verify(documents):
    n = len(documents)
    vocabulary = sorted({key for _, tf in documents for key in tf}, key=lambda key: key.encode())
    ordered = sorted(documents)
    with localcontext() as context:
        context.prec = 80
        dfs = {key: sum(key in tf for _, tf in documents) for key in vocabulary}
        idfs = {key: Decimal(1) + (Decimal(n+1)/Decimal(dfs[key]+1)).ln() for key in vocabulary}
        norms = {doc: sum(((Decimal(count)*idfs[key])**2 for key, count in tf.items()), Decimal(0)).sqrt() for doc, tf in documents}
        expected = [('S', str(n), str(sum(sum(tf.values()) for _, tf in documents)), str(sum(len(tf) for _, tf in documents)))]
        for doc, tf in ordered:
            expected.append(('D', str(doc), str(sum(tf.values())), str(len(tf)), norms[doc]))
        for term_id, key in enumerate(vocabulary):
            expected.append(('T', str(term_id), key.encode().hex(), str(dfs[key]), idfs[key]))
            for doc, tf in ordered:
                if key in tf:
                    expected.append(('P', str(term_id), str(doc), str(tf[key]), Decimal(tf[key])*idfs[key]/norms[doc]))
        payload = ''.join(str(doc)+''.join(' '+key.encode().hex()+':'+str(count) for key, count in tf.items())+'\n' for doc, tf in documents)
        result = subprocess.run([args.probe], input=payload, text=True, capture_output=True, check=True)
        lines = result.stdout.splitlines()
        check(lines[0] == 'SIFTWING-WEIGHTS 1 1', lines[0])
        check(result.stdout.endswith('\n') and '\r' not in result.stdout and len(lines) == len(expected)+1, lines)
        units = {doc: [] for doc, _ in documents}
        for line, want in zip(lines[1:], expected):
            fields = line.split(' ')
            check(len(fields) == len(want), (line, want))
            if want[0] == 'S':
                check(tuple(fields) == want, (line, want))
            else:
                check(tuple(fields[:-1]) == want[:-1], (line, want))
                value = number(fields[-1], want[-1])
                if want[0] == 'P':
                    check(0 < value <= 1, value)
                    units[int(fields[2])].append(value)
        for doc, tf in documents:
            check(math.isclose(math.fsum(w*w for w in units[doc]), 1.0 if tf else 0.0, rel_tol=1e-12, abs_tol=0.0), (doc, units[doc]))
        changed = list(reversed([(doc, dict(reversed(list(tf.items())))) for doc, tf in documents]))
        second_payload = ''.join(str(doc)+''.join(' '+key.encode().hex()+':'+str(count) for key, count in tf.items())+'\n' for doc, tf in changed)
        repeated = subprocess.run([args.probe], input=second_payload, text=True, capture_output=True, check=True)
        check(repeated.stdout == result.stdout, 'input permutation changed serialized bytes')
        transcripts.update(result.stdout.encode())

verify([])
verify([(0, {}), (2**64-1, {})])
verify([(0, {'a': 4, 'b': 3}), (7, {'a': 1}), (2**64-1, {})])
verify([(2**64-1, {'big': 2**64-2, 'tiny': 1})])
verify([(7, {'apple': 3, 'banana': 4})])
vocabulary = ['a', 'A', 'banana', '中文', '🙂', 'é', 'e\u0301', '\ufeff', '!']
for _ in range(60):
    ids = rng.sample([0, 1, 7, 42, 1000, 2**64-1], rng.randrange(7))
    verify([(doc, {key: rng.randrange(1, 1000) for key in rng.sample(vocabulary, rng.randrange(10))}) for doc in ids])
with localcontext() as context:
    context.prec = 80
    for n, df in [(1, 1), (7, 2), (2**64-1, 1), (2**64-1, 2**64-1), (2**64-1, 2**64-2), (2**53+1, 7)]:
        result = subprocess.run([args.probe, '--idf', str(n), str(df)], capture_output=True, text=True, check=True)
        number(result.stdout.strip(), Decimal(1)+(Decimal(n+1)/Decimal(df+1)).ln())
print(f'checks={checks} failures=0; 65 independent Decimal batches; serialized_sha256={transcripts.hexdigest()}')
