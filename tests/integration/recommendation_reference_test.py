"""原创独立参考：直接词字符集合求候选，全矩阵Levenshtein，不读取生产字符索引或DP行。"""
import argparse
import itertools
import json
import random
import subprocess

MAX = (1 << 64) - 1
LIMITS = [20000, 5000, MAX, 100000, 10000, 1000000, 10000000, 1000000]
SPACE = set(range(9, 14)) | {32, 133, 160, 5760, 8232, 8233, 8239, 8287, 12288} | set(range(8192, 8203))


def distance(a, b):
    # 完整二维矩阵，每格从上/左/对角读，独立于生产单行更新和短列交换。
    matrix = [[0] * (len(b) + 1) for _ in range(len(a) + 1)]
    for i in range(len(a) + 1):
        matrix[i][0] = i
    for j in range(len(b) + 1):
        matrix[0][j] = j
    for i, x in enumerate(a, 1):
        for j, y in enumerate(b, 1):
            matrix[i][j] = min(matrix[i - 1][j] + 1, matrix[i][j - 1] + 1,
                               matrix[i - 1][j - 1] + (x != y))
    return matrix[-1][-1]


def reference(raw, table, k, limits):
    def error(context):
        return {"error": "recommend." + context}
    qb, qs, mk, ml, mc, mb, mw, mo = limits
    if any(x == 0 for x in limits):
        return error("limits")
    if not 1 <= k <= mk:
        return error("top_k")
    if len(raw) > qb:
        return error("query_bytes")
    try:
        key = raw.decode("utf-8", "strict")
    except UnicodeDecodeError:
        return error("utf8")
    if "\0" in key:
        return error("nul")
    if not key or any(ord(x) in SPACE for x in key):
        return error("query")
    if len(key) > qs:
        return error("query_scalars")
    shared = set(key)
    # 关系数与候选数是不同计量；不消费生产索引/局部身份，直接遍历原创表证明。
    links = sum(len(shared & set(word)) for word in table)
    if links > ml:
        return error("character_links")
    words = [word for word in table if shared & set(word)]
    if len(words) > mc:
        return error("candidates")
    if sum(len(word.encode()) for word in words) > mb:
        return error("candidate_bytes")
    if sum(len(key) * len(word) for word in words) > mw:
        return error("work")
    ranked = sorted(words, key=lambda word: (distance(key, word), -table[word], word.encode()))[:k]
    if sum(len(word.encode()) for word in ranked) > mo:
        return error("output_bytes")
    ids = {word: i for i, word in enumerate(sorted(table, key=lambda word: word.encode()))}
    return {"terms": [[word.encode().hex(), ids[word], distance(key, word), table[word]] for word in ranked]}


def cases():
    result = []
    def add(key, table, k=MAX, limits=None):
        raw = key.encode() if isinstance(key, str) else key
        lim = list(LIMITS if limits is None else limits)
        result.append((raw, dict(table), k, lim))
    base = {"cat": 2, "bat": 5, "rat": 5, "act": 9, "dog": 100}
    for key in ["cat", "catcat", "act", "aa", "XYZ", "CAT", "", " "]:
        for k in [0, 1, 2, 3, 4, 10, MAX]:
            add(key, base, k)
    # 独立验算经典距离与编辑操作：交换两步，Unicode编码宽度不改变一次替换。
    for a, b, expected in [("kitten", "sitting", 3), ("中国", "中共", 1), ("😀a", "中a", 1),
                           ("cat", "act", 2), ("éa", "éa", 2), ("a", "ab", 1)]:
        assert distance(a, b) == expected
        add(a, {b: 1})
    alphabet = "ab中😀"
    words = ["".join(x) for length in [1, 2] for x in itertools.product(alphabet, repeat=length)]
    table = {word: 1 + (i % 3) for i, word in enumerate(words)}
    for key in words + ["".join(x) for x in itertools.product(alphabet, repeat=3)]:
        for k in [1, 3, MAX]:
            add(key, table, k)
    rng = random.Random(133)
    alphabet = "aAbB中国😀é́\u200d\ufeff!?"
    for _ in range(500):
        table = {"".join(rng.choices(alphabet, k=rng.randint(1, 8))): rng.randint(1, 100)
                 for _ in range(rng.randint(0, 18))}
        key = "".join(rng.choices(alphabet, k=rng.randint(1, 9)))
        add(key, table, rng.choice([1, 2, 5, MAX]))
    for cp in SPACE:
        add("a" + chr(cp), {"a": 1})
    for raw in [b"\xff", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"a\xe4\xb8",
                b"\x00\xff", b"\x00 ", b"a\x00", b" \xff"]:
        for k in [0, 1]:
            add(raw, base, k)
    for index in range(8):
        lim = LIMITS.copy(); lim[index] = 0
        add(b"\xff", base, 0, lim)
    # 对各计量的1/端点前/恰端点/端点后独立验证；小k也受完整候选/工作约束。
    for key, table in [("cat", base), ("中国", {"中国": 2, "国中": 3}),
                       ("aa", {"a": 3, "aa": 2, "ab": 4}), ("😀", {"😀中": 1}), ("a", {})]:
        sharing = [w for w in table if set(key) & set(w)]
        full = reference(key.encode(), table, 1, LIMITS)
        used = [len(key.encode()), len(key), 1, sum(len(set(key) & set(w)) for w in table),
                len(sharing), sum(len(w.encode()) for w in sharing), sum(len(key) * len(w) for w in sharing),
                sum(len(bytes.fromhex(item[0])) for item in full["terms"])]
        for index, value in enumerate(used):
            for n in sorted({0, 1, max(0, value - 1), value, value + 1, MAX}):
                lim = LIMITS.copy(); lim[index] = n
                add(key, table, 1, lim)
    add("a", {"a": MAX})
    add("aa", {"aqqqqqq": 1, "bb": 100})  # 全局更近bb也不入候选。
    add("a" * 300, {"a": 1, "ab": 2}, 1)
    add("😀" * 50, {"😀中": 1, "😀": 2}, MAX)
    return result


def run(probe):
    inputs = cases()
    lines = []
    expected = []
    for raw, table, k, lim in inputs:
        fields = [raw.hex() or "-", str(k), *map(str, lim)]
        fields.extend(word.encode().hex() + ":" + str(n) for word, n in table.items())
        lines.append(" ".join(fields))
        expected.append(reference(raw, table, k, lim))
    # 两个新进程输入顺序相反，避免进程缓存或上一次调用影响结果。
    for pass_id in range(2):
        order = list(range(len(lines)))
        if pass_id:
            order.reverse()
        process = subprocess.run([probe], input="\n".join(lines[i] for i in order) + "\n",
                                 text=True, capture_output=True, timeout=50, check=False)
        if process.returncode:
            raise AssertionError(f"probe exit={process.returncode}: {process.stderr}")
        output = process.stdout.splitlines()
        if len(output) != len(order):
            raise AssertionError(f"output size {len(output)} expected {len(order)}")
        for i, actual in zip(order, output):
            value = json.loads(actual)
            if value != expected[i]:
                raise AssertionError(f"case={i} input={inputs[i]!r} actual={value!r} expected={expected[i]!r}")
    print(f"recommendation independent cases={len(inputs)} processes=2 failures=0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    run(parser.parse_args().probe)
