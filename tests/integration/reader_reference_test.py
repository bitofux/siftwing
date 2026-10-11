#!/usr/bin/env python3
"""独立构造规范八文件，不调用Writer；重签哈希的反例必须仍被语义校验拒绝。"""
import argparse
import copy
import hashlib
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

NAMES = ["documents.bin", "offsets.bin", "search-lexicon.txt", "search-weights.txt",
         "recommendation-dictionary.txt", "recommendation-characters.txt"]
MAX = (1 << 64) - 1
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def sha(value):
    return hashlib.sha256(value).hexdigest()


def floathex(value):
    return struct.pack(">d", value).hex()


def record(identity, ordinal, kind, fields):
    flags = sum(1 << i for i, value in enumerate(fields[3:]) if value is not None)
    payload = b"".join(struct.pack(">Q", len(value or b"")) + (value or b"") for value in fields)
    header = struct.pack(">4sHHQQQBBHI", b"SWDR", 1, 40, 40 + len(payload), identity, ordinal, kind, flags, 0, 0)
    crc = zlib.crc32(header[:36] + payload)
    return header[:36] + struct.pack(">I", crc) + payload


def table(entries, size):
    payload = b"".join(struct.pack(">QQQ", *entry) for entry in entries)
    header = struct.pack(">4sHHQQII", b"SWDI", 1, 32, len(entries), size, 0, 0)
    return header[:24] + struct.pack(">I", zlib.crc32(header[:24] + header[28:] + payload)) + header[28:] + payload


def fixture(docs, recommendation, deliveries, version=0):
    # 文档字段含原始NUL/非UTF8与缺失/提供空；这些是已封板记录编码的合法值。
    docs = sorted(docs)
    terms = sorted({key for _, tf in docs for key in tf}, key=lambda s: s.encode())
    keys = sorted(recommendation, key=lambda s: s.encode())
    records, entries, library, fields = [], [], b"", []
    for d, (identity, _) in enumerate(docs):
        values = [b"" if d % 2 else "标题".encode(), b"body\0\xff", str(identity).encode(),
                  None if d % 2 else b"", b"author" if d % 3 else None, "日期".encode() if d % 2 else b""]
        fields.extend(len(v or b"") for v in values)
        encoded = record(identity, MAX if d % 2 else 0, 1 + d % 3, values)
        records.append((identity, encoded))
        entries.append((identity, len(library), len(encoded)))
        library += encoded
    total = sum(sum(tf.values()) for _, tf in docs)
    postings = sum(len(tf) for _, tf in docs)
    idfs = {key: 1 + math.log1p((len(docs) - sum(key in tf for _, tf in docs)) /
                              (sum(key in tf for _, tf in docs) + 1)) for key in terms}
    norms = {}
    for identity, tf in docs:
        norm = 0.0
        for key in terms:
            if key in tf:
                norm = math.hypot(norm, float(tf[key]) * idfs[key])
        norms[identity] = norm
    weights = "SIFTWING-WEIGHTS 1 1\nS %d %d %d\n" % (len(docs), total, postings)
    for identity, tf in docs:
        weights += "D %d %d %d %s\n" % (identity, sum(tf.values()), len(tf), floathex(norms[identity]))
    for t, key in enumerate(terms):
        weights += "T %d %s %d %s\n" % (t, key.encode().hex(), sum(key in tf for _, tf in docs), floathex(idfs[key]))
        for identity, tf in docs:
            if key in tf:
                weights += "P %d %d %d %s\n" % (t, identity, tf[key], floathex(float(tf[key]) * idfs[key] / norms[identity]))
    lexicon = "SIFTWING-LEXICON 1\nS %d\n" % len(terms)
    lexicon += "".join("T %d %s\n" % (i, key.encode().hex()) for i, key in enumerate(terms))
    rec_total = sum(recommendation.values())
    rec = "SIFTWING-RECOMMENDATION 1\nS %d %d %d\n" % (deliveries, rec_total, len(keys))
    rec += "".join("T %d %s %d\n" % (i, key.encode().hex(), recommendation[key]) for i, key in enumerate(keys))
    characters = {}
    for i, key in enumerate(keys):
        for scalar in sorted(set(map(ord, key))):
            characters.setdefault(scalar, []).append(i)
    characters = dict(sorted(characters.items()))
    links = sum(map(len, characters.values()))
    chars = "SIFTWING-CHARACTERS 1\nS %d %d\n" % (len(characters), links)
    chars += "".join("C %d %s\n" % (c, " ".join(map(str, ids))) for c, ids in characters.items())
    payloads = dict(zip(NAMES, [library, table(entries, len(library)), lexicon.encode(), weights.encode(), rec.encode(), chars.encode()]))
    s = (len(docs), len(terms), total, postings)
    r = (deliveries, len(keys), rec_total, len(characters), links)
    bounds = [max(fields, default=1), max((len(value) for _, value in records), default=1), len(docs), len(library), len(payloads[NAMES[1]]),
              0, max(map(len, payloads.values())), sum(map(len, payloads.values())), postings,
              sum(len(key.encode()) for _, tf in docs for key in tf), max((len(key.encode()) for key in terms), default=1), len(terms),
              sum(len(key.encode()) for key in terms), len(docs), max((sum(tf.values()) for _, tf in docs), default=1), total,
              max((sum(key in tf for _, tf in docs) for key in terms), default=1), len(keys), sum(len(k.encode()) for k in keys),
              max((len(k.encode()) for k in keys), default=1), len(keys), sum(len(k.encode()) for k in keys), deliveries, rec_total, len(characters), links]
    return dict(payloads=payloads, search=s, recommendation=r, records=records, keys=keys, frequencies=recommendation,
                characters=characters, version=version, bounds=bounds)


def manifest(f):
    text = "SIFTWING-SNAPSHOT 1\nI %d independent-v1\n" % f["version"]
    text += "S %d %d %d %d 1 %s %s\n" % (*f["search"], "a" * 64, "b" * 64)
    text += "R %d %d %d %d %d %s %s\n" % (*f["recommendation"], "c" * 64, "d" * 64)
    text += "".join("F %s 1 %d %s\n" % (name, len(f["payloads"][name]), sha(f["payloads"][name])) for name in NAMES)
    return text.encode()


def save(parent, f):
    leaf = parent / str(f["version"])
    leaf.mkdir()
    for name, value in f["payloads"].items():
        (leaf / name).write_bytes(value)
    meta = manifest(f)
    (leaf / "manifest.txt").write_bytes(meta)
    (leaf / "COMPLETE").write_text("SIFTWING-COMPLETE 1 %d %s\n" % (f["version"], sha(meta)))
    return leaf


def expected(f):
    text = "OK\nM %s\nW %s\n" % (manifest(f).hex(), f["payloads"][NAMES[3]].hex())
    text += "".join("D %d %s\n" % (i, value.hex()) for i, value in f["records"])
    text += "R %d %d %d\n" % (f["recommendation"][0], f["recommendation"][2], len(f["keys"]))
    text += "".join("T %d %s %d\n" % (i, k.encode().hex(), f["frequencies"][k]) for i, k in enumerate(f["keys"]))
    text += "".join("C %d %s\n" % (c, " ".join(map(str, ids))) for c, ids in f["characters"].items())
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--parent", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="reader-reference-", dir=args.parent) as temp:
        root = Path(temp)
        counter = 0

        def run(f, accepted, mutate=None, budget=None, path=None):
            nonlocal counter
            counter += 1
            parent = root / str(counter)
            parent.mkdir()
            leaf = save(parent, f)
            if mutate:
                mutate(leaf)
            before = {p.name: (p.lstat().st_mode, p.read_bytes()) for p in leaf.iterdir() if p.is_file() and not p.is_symlink()}
            command = [args.probe, str(path or parent), str(f["version"])]
            if budget:
                command += list(map(str, budget))
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            check(result.returncode == (0 if accepted else 1), (counter, command, result.returncode, result.stdout, result.stderr))
            if accepted:
                check(result.stdout == expected(f), (counter, "完整加载对象必须与独立参考一致", result.stdout))
            else:
                check(result.stdout.startswith("REJECT "), (counter, "拒绝须有诊断", result.stdout))
            after = {p.name: (p.lstat().st_mode, p.read_bytes()) for p in leaf.iterdir() if p.is_file() and not p.is_symlink()}
            check(before == after, (counter, "读取不得改写产物"))

        empty = fixture([], {}, 0)
        samples = [empty, fixture([(0, {})], {}, 9), fixture([(MAX, {"中😀": MAX})], {"推荐😀": MAX}, MAX, MAX),
                   fixture([(0, {"a": 3, "中": 4}), (77, {"a": 3, "中": 4})], {"abba": 7, "😀中": 9}, 5),
                   fixture([(0, {"a": 2}), (77, {}), (MAX, {"b": 3})], {"a": 1, "b": 2}, 17)]
        for f in samples:
            run(f, True)
        # 构造足够多的词表/身份/空支路，而非仅同一个Writer→Reader自证。
        for seed in range(24):
            key = ["z", "中文", "😀", "é", "e\u0301", "\ufeff", "𐀀", "!", "a+b"][seed % 9]
            docs = [(0, {key: seed + 1}), (MAX - seed, {})] if seed % 2 else [(77 + seed, {key: seed + 1})]
            rec = {key + key: seed + 2, "不同": seed + 3} if seed % 3 else {}
            run(fixture(docs, rec, seed + 7, seed + 1), True)
        basic = samples[3]
        for i in range(26):
            run(basic, False, budget=(i, 0))
            bound = len(manifest(basic)) if i == 5 else basic["bounds"][i]
            run(basic, True, budget=(i, bound))
            if bound > 1:
                run(basic, False, budget=(i, bound - 1))
        # 普通缺项/截断/单字节损坏/尾随，以及普通文件类型合同。
        for name in NAMES + ["manifest.txt", "COMPLETE"]:
            run(basic, False, lambda leaf, n=name: (leaf / n).unlink())
            for action in (lambda b: b[:-1], lambda b: b[:len(b)//2], lambda b: b + b"x", lambda b: bytes([b[0] ^ 1]) + b[1:]):
                run(basic, False, lambda leaf, n=name, a=action: (leaf / n).write_bytes(a((leaf / n).read_bytes())))
            def directory(leaf, n=name):
                (leaf / n).unlink()
                (leaf / n).mkdir()
            run(basic, False, directory)
            def symbolic(leaf, n=name):
                (leaf / n).rename(leaf.parent / "external-file")
                (leaf / n).symlink_to(leaf.parent / "external-file")
            run(basic, False, symbolic)
        run(basic, False, lambda leaf: (leaf / "extra").write_bytes(b""))
        run(basic, False, lambda leaf: (leaf / "extra").mkdir())
        import os
        def fifo(leaf):
            (leaf / "documents.bin").unlink()
            os.mkfifo(leaf / "documents.bin")
        run(basic, False, fifo)
        # 文件SHA和COMPLETE均重签：格式与关系错误仍必须拒绝。
        mutants = []
        for name in NAMES[2:]:
            for replace in (lambda b: b.replace(b" 1\n", b" 2\n", 1), lambda b: b.replace(b"\n", b"\r\n"),
                            lambda b: b + b"\n", lambda b: b.replace(b"S ", b"S  ", 1)):
                f = copy.deepcopy(basic)
                f["payloads"][name] = replace(f["payloads"][name])
                if f["payloads"][name] != basic["payloads"][name]:
                    mutants.append(f)
        for name, old, new in [(NAMES[2], b"T 0 61", b"T 0 62"), (NAMES[2], b"T 1 ", b"T 0 "),
                               (NAMES[3], b"P 0 0 3 ", b"P 0 9 3 "), (NAMES[3], b"P 0 0 3 ", b"P 0 0 4 "),
                               (NAMES[3], b"3fe3333333333333", b"7ff8000000000000"),
                               (NAMES[3], b"D 0 7 2 ", b"D 0 7 1 "), (NAMES[3], b"S 2 14 4", b"S 2 15 4"),
                               (NAMES[4], b"T 0 61626261 7", b"T 0 61626261 8"),
                               (NAMES[4], b"T 0 61626261 7", b"T 0 612062 7"),
                               (NAMES[5], b"C 97 0", b"C 98 0"), (NAMES[5], b"C 97 0", b"C 97 1"),
                               (NAMES[5], b"C 97 0", b"C 97 0 0"), (NAMES[5], b"C 97 0", b"C 55296 0")]:
            f = copy.deepcopy(basic)
            check(old in f["payloads"][name], (name, old))
            f["payloads"][name] = f["payloads"][name].replace(old, new, 1)
            mutants.append(f)
        for name in NAMES[:2]:
            for offset in ((0, 4, 16, 32, 34) if name == NAMES[0] else (0, 4, 16, 28, 32)):
                f = copy.deepcopy(basic)
                data = bytearray(f["payloads"][name]); data[offset] ^= 1
                if name == NAMES[0]:
                    length = struct.unpack_from(">Q", data, 8)[0]
                    struct.pack_into(">I", data, 36, zlib.crc32(data[:36] + data[40:length]))
                else:
                    struct.pack_into(">I", data, 24, zlib.crc32(data[:24] + data[28:]))
                f["payloads"][name] = bytes(data)
                mutants.append(f)
        for f in mutants:
            run(f, False)
        # 不支持manifest版本/角色版本，marker错误及目录身份不符。
        for old, new in [(b"SIFTWING-SNAPSHOT 1", b"SIFTWING-SNAPSHOT 2"), (b"I 0 ", b"I 1 "),
                         (b"F offsets.bin 1 ", b"F offsets.bin 2 "), (b"F documents.bin", b"F unknown.bin")]:
            def change(leaf, a=old, b=new):
                value = (leaf / "manifest.txt").read_bytes().replace(a, b, 1)
                (leaf / "manifest.txt").write_bytes(value)
                (leaf / "COMPLETE").write_text("SIFTWING-COMPLETE 1 0 %s\n" % sha(value))
            run(basic, False, change)
        run(basic, False, lambda leaf: (leaf / "COMPLETE").write_text("SIFTWING-COMPLETE 1 1 %s\n" % sha(manifest(basic))))
        parent = root / "path-fixture"; parent.mkdir(); save(parent, basic)
        linked = root / "linked-parent"; linked.symlink_to(parent, target_is_directory=True)
        result = subprocess.run([args.probe, str(linked), "0"], capture_output=True, text=True)
        check(result.returncode == 1, "父路径链接拒绝")
        result = subprocess.run([args.probe, str(parent / ".." / parent.name), "0"], capture_output=True, text=True)
        check(result.returncode == 1, "..组件拒绝")
        other = root / "linked-version"; other.mkdir(); (other / "0").symlink_to(parent / "0", target_is_directory=True)
        result = subprocess.run([args.probe, str(other), "0"], capture_output=True, text=True)
        check(result.returncode == 1, "版本叶子链接拒绝")
        result = subprocess.run([args.probe, str(parent / ".") + "///", "0"], capture_output=True, text=True)
        check(result.returncode == 0, "点与多余斜杠合法")
    print("reader independent checks=%d failures=0 fixtures=%d" % (checks, counter))


if __name__ == "__main__":
    main()
