"""用原创输入及独立struct/zlib/hashlib、Unicode集合和数学参考核真实八文件；无生产Reader。"""
import argparse
import hashlib
import math
import pathlib
import random
import re
import struct
import subprocess
import tempfile
import zlib

CHECKS = 0
MAX_ID = (1 << 64) - 1
NAMES = ["documents.bin", "offsets.bin", "search-lexicon.txt", "search-weights.txt",
         "recommendation-dictionary.txt", "recommendation-characters.txt"]


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def encode_record(document):
    identity, title, body, _ = document
    url = ("https://original.invalid/" + str(identity)).encode() if identity % 2 else b""
    published = b"2026-10-11" if identity % 5 == 0 else b""
    flags = int(identity % 2 != 0) | (int(identity % 3 == 0) << 1) | (int(identity % 5 == 0) << 2)
    fields = [title, body, ("original/" + str(identity) + ".txt").encode(), url, b"", published]
    payload = b"".join(struct.pack(">Q", len(value)) + value for value in fields)
    header = struct.pack(">4sHHQQQBBH", b"SWDR", 1, 40, 40 + len(payload), identity, identity % 19, 1, flags, 0)
    return header + struct.pack(">I", zlib.crc32(header + payload)) + payload


def offset_bytes(documents, library):
    offset = 0
    rows = []
    for document in documents:
        size = len(encode_record(document))
        rows.append(struct.pack(">QQQ", document[0], offset, size))
        offset += size
    head = struct.pack(">4sHHQQ", b"SWDI", 1, 32, len(documents), len(library))
    tail = b"\0" * 4 + b"".join(rows)
    return head + struct.pack(">I", zlib.crc32(head + tail)) + tail


def text_bytes(lines):
    return ("\n".join(lines) + "\n").encode("ascii")


def fp(token):
    check(re.fullmatch("[0-9a-f]{16}", token) is not None, "canonical binary64 bits")
    value = struct.unpack(">d", bytes.fromhex(token))[0]
    check(math.isfinite(value) and value >= 0, "finite nonnegative numeric value")
    return value


def near(actual, expected):
    check(math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12), "independent math reference")


def check_weights(payload, documents, terms):
    check(payload.endswith(b"\n") and b"\r" not in payload, "canonical weight line ending")
    lines = payload.decode("ascii").splitlines()
    # 重建原统计/关系，不调用生产序列化；浮点用独立数学容差，原始整数精确核。
    raw = {document[0]: document[3] for document in documents}
    total = sum(sum(tf.values()) for tf in raw.values())
    postings = sum(len(tf) for tf in raw.values())
    check(lines[:2] == ["SIFTWING-WEIGHTS 1 1", f"S {len(documents)} {total} {postings}"], "weight header/statistics")
    idfs = {}
    norms = {identity: 0.0 for identity in raw}
    for word in terms:
        frequency = sum(word in tf for tf in raw.values())
        idfs[word] = 1 + math.log1p((len(documents) - frequency) / (frequency + 1))
        for identity, tf in raw.items():
            if word in tf:
                norms[identity] = math.hypot(norms[identity], float(tf[word]) * idfs[word])
    position = 2
    for identity, _, _, tf in documents:
        row = lines[position].split(" ")
        check(row[:4] == ["D", str(identity), str(sum(tf.values())), str(len(tf))] and len(row) == 5, "D exact identity/statistics")
        near(fp(row[4]), norms[identity])
        if not tf:
            check(row[4] == "0000000000000000", "empty document exact positive zero")
        position += 1
    for term_id, word in enumerate(terms):
        row = lines[position].split(" ")
        frequency = sum(word in tf for tf in raw.values())
        check(row[:4] == ["T", str(term_id), word.hex(), str(frequency)] and len(row) == 5, "T exact key/local ID/DF")
        near(fp(row[4]), idfs[word])
        position += 1
        for identity, tf in raw.items():
            if word not in tf:
                continue
            row = lines[position].split(" ")
            check(row[:4] == ["P", str(term_id), str(identity), str(tf[word])] and len(row) == 5, "P exact identity/raw count")
            near(fp(row[4]), float(tf[word]) * idfs[word] / norms[identity])
            position += 1
    check(position == len(lines), "no missing or extra weight relationships")
    check(text_bytes(lines) == payload, "canonical weight bytes round trip by independent grammar")


def reference(folder, version, source_documents, rec_documents, rec):
    documents = sorted(source_documents)
    terms = sorted({word for document in documents for word in document[3]})
    rec_terms = sorted(rec)
    character_ids = {}
    for identity, word in enumerate(rec_terms):
        for scalar in set(map(ord, word.decode("utf-8"))):
            character_ids.setdefault(scalar, []).append(identity)
    links = sum(map(len, character_ids.values()))
    library = b"".join(encode_record(document) for document in documents)
    offsets = offset_bytes(documents, library)
    lexicon = text_bytes(["SIFTWING-LEXICON 1", f"S {len(terms)}"] +
                         [f"T {identity} {word.hex()}" for identity, word in enumerate(terms)])
    dictionary = text_bytes(["SIFTWING-RECOMMENDATION 1", f"S {rec_documents} {sum(rec.values())} {len(rec_terms)}"] +
                           [f"T {identity} {word.hex()} {rec[word]}" for identity, word in enumerate(rec_terms)])
    characters = text_bytes(["SIFTWING-CHARACTERS 1", f"S {len(character_ids)} {links}"] +
                           ["C " + str(scalar) + " " + " ".join(map(str, character_ids[scalar])) for scalar in sorted(character_ids)])
    weights = (folder / "search-weights.txt").read_bytes()
    check_weights(weights, documents, terms)
    expected = [library, offsets, lexicon, weights, dictionary, characters]
    check(sorted(path.name for path in folder.iterdir()) == sorted(NAMES + ["manifest.txt", "COMPLETE"]), "exact eight file set")
    for name, payload in zip(NAMES, expected):
        path = folder / name
        check(path.is_file() and not path.is_symlink() and path.read_bytes() == payload, "independent payload bytes: " + name)
    sha = lambda value: hashlib.sha256(value).hexdigest()
    total = sum(sum(document[3].values()) for document in documents)
    postings = sum(len(document[3]) for document in documents)
    manifest = text_bytes(["SIFTWING-SNAPSHOT 1", f"I {version} original-build-1",
        f"S {len(documents)} {len(terms)} {total} {postings} 1 {sha(b'selected-search:v1')} {sha(b'search-config:v1')}",
        f"R {rec_documents} {len(rec_terms)} {sum(rec.values())} {len(character_ids)} {links} {sha(b'selected-recommendation:v1')} {sha(b'recommendation-config:v1')}"] +
        [f"F {name} 1 {len(payload)} {sha(payload)}" for name, payload in zip(NAMES, expected)])
    check((folder / "manifest.txt").read_bytes() == manifest, "independent complete manifest statistics/sizes/hashes")
    check((folder / "COMPLETE").read_bytes() == text_bytes([f"SIFTWING-COMPLETE 1 {version} {sha(manifest)}"]), "independent marker binding")
    return expected + [manifest, (folder / "COMPLETE").read_bytes()]


def wire(documents, rec_documents, rec):
    token = lambda value: value.hex() or "-"
    lines = [str(len(documents))]
    for identity, title, body, tf in documents:
        lines.append(f"{identity} {token(title)} {token(body)} {len(tf)}")
        lines.extend(f"{token(word)} {count}" for word, count in tf.items())
    lines.append(f"{rec_documents} {len(rec)}")
    lines.extend(f"{token(word)} {count}" for word, count in rec.items())
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--parent", required=True)
    args = parser.parse_args()
    generator = random.Random(1242026)
    words = [word.encode() for word in ["a", "A", "blue", "red", "重复", "😀😀", "𐀀", "é", "e\u0301", "zzzzz", "99", "a+b"]]
    cases = [([], 0, {}), ([], 7, {}), ([(0, b"", b"", {})], 3, {b"red": 4}),
             ([(MAX_ID, "原创".encode(), b"a\0a", {b"a": 2})], 0, {})]
    for index in range(44):
        count = generator.randrange(6)
        ids = generator.sample([0, MAX_ID, 7, 18, 19, 200, 1000], count)
        documents = []
        for identity in ids:
            tf = {word: generator.randrange(1, 50) for word in generator.sample(words, generator.randrange(6))}
            title = ("原创标题-" + str(identity)).encode()
            body = b"body\0" + b" ".join(tf)
            documents.append((identity, title, body, tf))
        rec_documents = generator.randrange(9)
        rec = {} if rec_documents == 0 else {word: generator.randrange(1, 100) for word in generator.sample(words, generator.randrange(8))}
        cases.append((documents, rec_documents, rec))
    aggregate = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="writer-reference-", dir=args.parent) as temporary:
        root = pathlib.Path(temporary)
        for index, (documents, rec_documents, rec) in enumerate(cases):
            version = [0, MAX_ID, 42][index % 3]
            parent = root / str(index)
            parent.mkdir()
            result = subprocess.run([args.probe, str(parent), str(version)], input=wire(documents, rec_documents, rec), text=True, capture_output=True, timeout=20)
            check(result.returncode == 0, "probe publication: " + result.stderr)
            check([path.name for path in parent.iterdir()] == [str(version)], "no leftover on successful publication")
            payloads = reference(parent / str(version), version, documents, rec_documents, rec)
            for payload in payloads:
                aggregate.update(struct.pack(">Q", len(payload)))
                aggregate.update(payload)
            # 独立置换记录和词频输入顺序，不改变来源元数据；第二正式目录字节逐一相同。
            permuted = [(identity, title, body, dict(reversed(list(tf.items())))) for identity, title, body, tf in reversed(documents)]
            second = root / (str(index) + "-permuted")
            second.mkdir()
            repeated = subprocess.run([args.probe, str(second), str(version)], input=wire(permuted, rec_documents, dict(reversed(list(rec.items())))), text=True, capture_output=True, timeout=20)
            check(repeated.returncode == 0, "permutation publication: " + repeated.stderr)
            for name, payload in zip(NAMES + ["manifest.txt", "COMPLETE"], payloads):
                check((second / str(version) / name).read_bytes() == payload, "deterministic permutation: " + name)
    print(f"checks={CHECKS} failures=0; 48 independent complete snapshots and permutations; canonical_sha256={aggregate.hexdigest()}")


if __name__ == "__main__":
    main()
