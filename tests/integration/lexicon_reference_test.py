"""独立Python bytes排序与Unicode标量集合，验证局部ID、累计统计及确定性；原创固定种子。"""
import argparse
import json
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
probe = parser.parse_args().probe
checks = 0


def check(value, label):
    global checks
    checks += 1
    if not value:
        raise RuntimeError(label)


def run(mode, words, args=(), code=0):
    if mode == 'lex':
        data = ''.join(word.hex() + '\n' for word in words)
    else:
        data = ''.join(str(count) + ' ' + word.hex() + '\n' for word, count in words.items())
    result = subprocess.run([probe, mode] + list(map(str, args)), input=data,
                            text=True, capture_output=True, timeout=5)
    check(result.returncode == code, (mode, args, result.returncode, result.stdout, result.stderr))
    return json.loads(result.stdout)


def lexical(words):
    unique = sorted(set(words))
    return {'terms': [[word.hex(), i] for i, word in enumerate(unique)],
            'reverse': [word.hex() for word in unique]}


def recommendation(words, documents):
    expected = lexical(words)
    ordered = sorted(words)
    characters = {}
    for i, word in enumerate(ordered):
        for cp in set(map(ord, word.decode('utf-8'))):
            characters.setdefault(cp, []).append(i)
    expected.update(documents=documents, total=sum(words.values()),
                    frequencies=[words[word] for word in ordered],
                    characters=[[cp, characters[cp]] for cp in sorted(characters)])
    return expected


rng = random.Random(119)
alphabet = ['a', 'b', 'A', '0', '#', '中', '词', 'é', 'e', '\u0301', '😀', '\U0010ffff', '\ufeff', '\u200b']
for case in range(72):
    words = [''.join(rng.choice(alphabet) for _ in range(rng.randrange(1, 12))).encode()
             for _ in range(case % 21)]
    words += words[:3]
    expected = lexical(words)
    check(run('lex', words) == expected, ('independent lexical reference', case))
    rng.shuffle(words)
    check(run('lex', words) == expected, ('permutation deterministic', case))
    frequencies = {word: rng.randrange(1, 100) for word in words}
    documents = case % 7 + 1
    expected_rec = recommendation(frequencies, documents)
    check(run('rec', frequencies, [documents, sum(frequencies.values())]) == expected_rec,
          ('scalar/frequency reference', case))
    reversed_table = dict(reversed(list(frequencies.items())))
    check(run('rec', reversed_table, [documents, sum(frequencies.values())]) == expected_rec,
          ('recommendation order deterministic', case))

words = [b'a', b'b', '中中'.encode(), '😀a'.encode(), '中中'.encode()]
limits = [5, 19, 6, 4, 13]
check(run('lex', words, limits) == lexical(words), 'exact five budgets')
for field in range(5):
    below = limits.copy()
    below[field] -= 1
    run('lex', words, below, 1)
    zero = limits.copy()
    zero[field] = 0
    check(run('lex', [], zero, 1)['error'] == 'lexicon.limits', 'empty config priority')

frequencies = {b'a': 2, b'b': 1, '中中'.encode(): 3, '😀a'.encode(): 4}
rlimits = [4, 13, 6, 4, 13, 2, 10, 4, 5]
check(run('rec', frequencies, [2, 10] + rlimits) == recommendation(frequencies, 2), 'nine exact budgets')
for field in range(9):
    below = rlimits.copy()
    below[field] -= 1
    run('rec', frequencies, [2, 10] + below, 1)
    zero = rlimits.copy()
    zero[field] = 0
    check(run('rec', {}, [0, 0] + zero, 1)['error'] == 'dictionary.limits', 'nine config priority')

for invalid in [b'\x00', b'a\x00b', b'\x80', b'\xc0\xaf', b'\xe2\x82', b'\xed\xa0\x80',
                b'\xf4\x90\x80\x80', b'\xff', b'', b'a b']:
    run('lex', [invalid], code=1)
    run('rec', {invalid: 1}, [1, 1], 1)
# White_Space明确列出，避免依赖Python isspace的额外控制字符集合。
spaces = list(range(9, 14)) + [32, 0x85, 0xa0, 0x1680] + list(range(0x2000, 0x200b)) + [0x2028, 0x2029, 0x202f, 0x205f, 0x3000]
for cp in spaces:
    word = ('x' + chr(cp)).encode()
    check(run('lex', [word], code=1)['error'] == 'lexicon.term', 'Unicode White_Space rejected')
    run('rec', {word: 1}, [1, 1], 1)
run('rec', {b'a': 0}, [1, 0], 1)
run('rec', {b'a': 1}, [0, 1], 1)
run('rec', {b'a': 1}, [1, 2], 1)
check(run('rec', {}, [2, 0]) == recommendation({}, 2), 'empty deliveries retained')
maximum = 2**64 - 1
unlimited = [1000, 1048576, 4096, 1000, 1048576, maximum, maximum, 100000, 100000]
check(run('rec', {b'a': maximum}, [maximum, maximum] + unlimited) == recommendation({b'a': maximum}, maximum),
      'full uint64 counters')
check(run('rec', {b'a': maximum, b'b': 1}, [1, maximum] + unlimited, 1)['error'] == 'dictionary.overflow',
      'frequency sum overflow before publication')
print(f'checks={checks} failures=0; independent byte ordering, scalar sets and recommendation counts')
