#!/usr/bin/env python3
"""原创固定数据到正式应用的V1验收；黄金词界人工指定，数学/格式参考独立。

不导入生产或既有测试参考，不反推期望；全部输入复制到独立临时根，原fixture只读。
生命周期探针验证同一进程的fd交接，进程退出不充当fd/heap正确性的证据。
"""
import argparse
from collections import Counter
import hashlib
import html
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zlib

CHECKS = 0
PROCESSES = Counter()
FILES = ["documents.bin", "offsets.bin", "search-lexicon.txt", "search-weights.txt",
         "recommendation-dictionary.txt", "recommendation-characters.txt", "manifest.txt", "COMPLETE"]


def check(value, message):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(message)


def sha(value):
    return hashlib.sha256(value).hexdigest()


def inventory(root):
    return {p.relative_to(root).as_posix(): sha(p.read_bytes())
            for p in root.rglob("*") if p.is_file() and not p.is_symlink()}


def invoke(kind, args, code=0, stdout=subprocess.PIPE):
    PROCESSES[kind] += 1
    p = subprocess.run([getattr(OPTIONS, kind), *map(str, args)], capture_output=False,
                       stdout=stdout, stderr=subprocess.PIPE, timeout=40)
    check(p.returncode == code, (kind, args, p.returncode, p.stdout, p.stderr))
    # 正常读取warning不掩盖sanitizer诊断，预期失败也不能误接纳检测器报错。
    check(not any(marker in p.stderr for marker in
                  [b"AddressSanitizer", b"LeakSanitizer", b"UndefinedBehaviorSanitizer", b"runtime error:"]),
          ("sanitizer diagnostic", p.stderr))
    if code == 0 and kind != "indexer":
        check(p.stderr == b"", ("successful process diagnostic", p.stderr))
    return p


def resources(language):
    flags = ["--stopwords", DATA / "stopwords.txt"]
    if language == "chinese":
        flags += ["--tokenizer", "cppjieba", "--dictionary", DATA / "dictionary.txt",
                  "--model", DATA / "hmm.txt", "--hmm", "off"]
    return flags


def build(root, name, search="english", recommendation=None, version=0, reverse=False, rec_subset=None):
    recommendation = recommendation or search
    parent = root / name
    parent.mkdir()
    args = ["--output", parent, "--version", version, "--build-version", "original-v1-golden-1",
            "--search-dedup-distance", 0]
    sources = sorted((root / "input").rglob("*"))
    sources = [p for p in sources if p.is_file()]
    if reverse:
        sources.reverse()
    for role, language in [("search", search), ("recommendation", recommendation)]:
        for path in sources:
            if role == "recommendation" and rec_subset is not None and path.name not in rec_subset:
                continue
            args += [f"--{role}-{'rss' if path.suffix == '.xml' else 'txt'}", path]
        flags = resources(language)
        args += ["--" + role + "-" + x[2:] if str(x).startswith("--") else x for x in flags]
    output = invoke("indexer", args).stdout.decode()
    report = dict(line.split("=", 1) for line in output.splitlines())
    check(report["durability"] == "synced", "formal Writer acknowledged synchronization")
    return parent, report


def query(parent, text, language="english", operation="SEARCH", version=0, extra=(), code=0):
    args = [operation, "--snapshot", parent, "--version", version,
            "--query" if operation == "SEARCH" else "--key", text]
    if operation == "SEARCH":
        args += resources(language)
    p = invoke("cli", [*args, *extra], code)
    if code:
        check(p.stdout == b"", "ordinary pre-output failure has no partial JSON")
        return p
    check(p.stdout.endswith(b"\n") and p.stdout.count(b"\n") == 1, "exactly one complete JSON line")
    value = json.loads(p.stdout.decode(), parse_constant=lambda v: check(False, v))
    check(value["schema_version"] == 1 and value["snapshot_version"] == str(version), "schema/version contract")
    return value, p.stdout


def selected(language):
    return [r for r in GOLDEN["records"] if r["id"] in GOLDEN["kept"][language]]


def accumulated(language, subset=None):
    result = Counter()
    for record in GOLDEN["records"]:
        if subset is None or Path(record["source"]).name in subset:
            result.update(record[language])
    return result


def decoded_records(data):
    result = []
    while data:
        check(len(data) >= 40, "complete fixed record header")
        magic, version, header, length, identity, ordinal, kind, flags, reserved, crc = struct.unpack(
            ">4sHHQQQBBHI", data[:40])
        check((magic, version, header, reserved) == (b"SWDR", 1, 40, 0), "record independent header")
        record, data = data[:length], data[length:]
        check(len(record) == length and zlib.crc32(record[:36] + record[40:]) == crc, "complete record/CRC")
        position, fields = 40, []
        for _ in range(6):
            size = struct.unpack(">Q", record[position:position+8])[0]
            position += 8
            fields.append(record[position:position+size].decode())
            position += size
        check(position == length, "six fields cover complete record")
        result.append((identity, ordinal, kind, flags, fields))
    return result


def verify_snapshot(root, parent, language, rec_language=None, subset=None):
    rec_language = rec_language or language
    leaf = parent / "0"
    check(sorted(p.name for p in leaf.iterdir()) == sorted(FILES), "exact eight-file formal publication")
    expected = selected(language)
    decoded = decoded_records((leaf / "documents.bin").read_bytes())
    check([r[0] for r in decoded] == [r["id"] for r in expected], "frozen sparse kept identities")
    for (identity, ordinal, kind, flags, fields), record in zip(decoded, expected):
        check(identity == record["id"] and ordinal == record["ordinal"], "original RSS ordinal preserved")
        check(kind == (2 if record["source"].startswith("rss/") else 1), "explicit source kind")
        check(fields[:3] == [record["title"], record["body"], str(root / "input" / record["source"])[1:]],
              "manual extraction/title/source golden")
        check(fields[3] == (record["url"] or "") and bool(flags & 1) == (record["url"] is not None),
              "original URL and optional presence")
    weights = [line.split() for line in (leaf / "search-weights.txt").read_text().splitlines()]
    tfs = {r["id"]: Counter(r[language]) for r in expected}
    terms = sorted({term for tf in tfs.values() for term in tf}, key=lambda s: s.encode())
    check(weights[0] == ["SIFTWING-WEIGHTS", "1", "1"], "frozen weighting policy")
    check(list(map(int, weights[1][1:])) == [len(expected), sum(sum(tf.values()) for tf in tfs.values()),
                                          sum(len(tf) for tf in tfs.values())], "N/TF/posting totals")
    idfs = {term: 1 + math.log((len(expected)+1)/(sum(term in tf for tf in tfs.values())+1)) for term in terms}
    norms = {identity: math.sqrt(sum((count*idfs[term])**2 for term, count in tf.items())) for identity, tf in tfs.items()}
    def floating(value):
        return struct.unpack(">d", bytes.fromhex(value))[0]
    rows = [row for row in weights if row[0] == "T"]
    check([bytes.fromhex(row[2]).decode() for row in rows] == terms, "independent vocabulary byte order")
    for row in rows:
        term = terms[int(row[1])]
        check(int(row[3]) == sum(term in tf for tf in tfs.values()), "independent DF")
        check(math.isclose(floating(row[4]), idfs[term], rel_tol=3e-15), "independent mathematical IDF")
    actual_postings = {(int(row[1]), int(row[2])): int(row[3]) for row in weights if row[0] == "P"}
    check(actual_postings == {(terms.index(term), identity): count for identity, tf in tfs.items()
                             for term, count in tf.items()}, "all original TF/posting relations")
    for row in weights:
        if row[0] == "D":
            identity = int(row[1])
            check(list(map(int, row[2:4])) == [sum(tfs[identity].values()), len(tfs[identity])], "document totals")
            check(math.isclose(floating(row[4]), norms[identity], rel_tol=3e-15), "independent document norm")
        if row[0] == "P":
            term, identity = terms[int(row[1])], int(row[2])
            check(math.isclose(floating(row[4]), tfs[identity][term]*idfs[term]/norms[identity], rel_tol=3e-15),
                  "independent normalized document weight")
    frequencies = accumulated(rec_language, subset)
    rows = [line.split() for line in (leaf / "recommendation-dictionary.txt").read_text().splitlines()]
    check({bytes.fromhex(row[2]).decode(): int(row[3]) for row in rows[2:]} == frequencies,
          "recommendation counts duplicates and its independent delivered selection")
    count = sum(subset is None or Path(r["source"]).name in subset for r in GOLDEN["records"])
    check(list(map(int, rows[1][1:])) == [count, sum(frequencies.values()), len(frequencies)],
          "empty TF delivery counts separately from no_text")
    manifest = (leaf / "manifest.txt").read_text().splitlines()
    for line in manifest[4:]:
        _, name, version, size, digest = line.split()
        data = (leaf / name).read_bytes()
        check(version == "1" and len(data) == int(size) and sha(data) == digest, "independent payload SHA/size")
    check((leaf / "COMPLETE").read_text() == "SIFTWING-COMPLETE 1 0 " + sha((leaf / "manifest.txt").read_bytes()) + "\n",
          "completion binds complete manifest bytes")


def display(body, terms, window=160, leading=40):
    folded = lambda s: "".join(chr(ord(c)+32) if "A" <= c <= "Z" else c for c in s)
    positions = [folded(body).find(folded(t)) for t in terms if folded(t) in folded(body)]
    size = min(len(body), window)
    start = min(max(0, min(positions)-leading), len(body)-size) if positions else 0
    text = body[start:start+size]
    intervals = [(i, i+len(t)) for t in terms for i in range(len(text)) if folded(text).startswith(folded(t), i)]
    active = {i for a, b in intervals for i in range(a, b)}
    spans, chunks, i = [], [], 0
    while i < len(text):
        j = i+1
        while j < len(text) and (j in active) == (i in active):
            j += 1
        escaped = html.escape(text[i:j], quote=True).replace("&#x27;", "&#39;")
        if i in active:
            spans.append({"byte_offset": len(text[:i].encode()), "byte_length": len(text[i:j].encode())})
            escaped = "<mark>" + escaped + "</mark>"
        chunks.append(escaped)
        i = j
    return {"text": text, "source_byte_offset": len(body[:start].encode()), "matched": bool(positions),
            "truncated_before": start > 0, "truncated_after": start+size < len(body),
            "html": "".join(chunks), "spans": spans}


def verify_search(parent, language, text, tokens, mode="or", k=10, window=160):
    out, raw = query(parent, text, language, extra=["--mode", mode, "--top-k", k,
                    "--summary-window", window, "--summary-context", 0])
    records = selected(language)
    tfs = {r["id"]: Counter(r[language]) for r in records}
    q = Counter(tokens)
    check(out["terms"] == [{"term": t, "tf": q[t]} for t in sorted(q, key=lambda s: s.encode())], "manual query TF")
    vocabulary = {t for tf in tfs.values() for t in tf}
    idf = {t: 1+math.log((len(records)+1)/(sum(t in tf for tf in tfs.values())+1)) for t in vocabulary}
    known = {t: n*idf[t] for t, n in q.items() if t in idf}
    expected = []
    if known and not (mode == "and" and any(t not in idf for t in q)):
        qnorm = math.sqrt(sum(x*x for x in known.values()))
        for record in records:
            tf = tfs[record["id"]]
            if not (all(t in tf for t in q) if mode == "and" else any(t in tf for t in q)):
                continue
            norm = math.sqrt(sum((count*idf[t])**2 for t, count in tf.items()))
            score = min(1, sum(value*tf.get(t, 0)*idf[t] for t, value in known.items())/(qnorm*norm))
            expected.append((record["id"], score))
    expected.sort(key=lambda p: (-p[1], p[0]))
    expected = expected[:k]
    check([int(r["document_id"]) for r in out["results"]] == [i for i, _ in expected], "independent ranked identities")
    for hit, (identity, score) in zip(out["results"], expected):
        record = next(r for r in records if r["id"] == identity)
        check(math.isclose(hit["score"], score, abs_tol=3e-15, rel_tol=3e-15), "independent cosine")
        check(hit["title"] == record["title"] and hit["url"] == record["url"], "metadata from original identity")
        check(hit["summary"] == display(record["body"], q, window, 0), "independent Unicode window and escaped union")
    return out, raw


def distance(a, b):
    matrix = [[0]*(len(b)+1) for _ in range(len(a)+1)]
    for i in range(len(a)+1):
        matrix[i][0] = i
    for j in range(len(b)+1):
        matrix[0][j] = j
    for i in range(1, len(a)+1):
        for j in range(1, len(b)+1):
            matrix[i][j] = min(matrix[i-1][j]+1, matrix[i][j-1]+1, matrix[i-1][j-1]+(a[i-1] != b[j-1]))
    return matrix[-1][-1]


def verify_suggest(parent, language, key, subset=None):
    out, raw = query(parent, key, operation="SUGGEST")
    frequencies = accumulated(language, subset)
    vocabulary = sorted(frequencies, key=lambda s: s.encode())
    expected = sorted((distance(key, term), -count, term) for term, count in frequencies.items() if set(key) & set(term))[:10]
    check(out["results"] == [{"term_id": str(vocabulary.index(term)), "term": term, "distance": d, "frequency": -count}
                              for d, count, term in expected], "independent character recall/distance/frequency order")
    return out, raw


def rebind(leaf):
    lines = (leaf / "manifest.txt").read_text().splitlines()
    for i in range(4, 10):
        name = lines[i].split()[1]
        data = (leaf / name).read_bytes()
        lines[i] = f"F {name} 1 {len(data)} {sha(data)}"
    (leaf / "manifest.txt").write_text("\n".join(lines)+"\n")
    (leaf / "COMPLETE").write_text("SIFTWING-COMPLETE 1 0 "+sha((leaf / "manifest.txt").read_bytes())+"\n")


def semantic_copy(parent, root):
    copied = root / "semantic-corrupt"
    shutil.copytree(parent, copied)
    leaf = copied / "0"
    path = leaf / "recommendation-dictionary.txt"
    lines = path.read_text().splitlines()
    fields = lines[2].split()
    check(fields[0:2] == ["T", "0"], "first canonical recommendation identity")
    fields[2] = "aaaaaa".encode().hex()
    lines[2] = " ".join(fields)
    path.write_text("\n".join(lines)+"\n")
    rebind(leaf)
    return copied


def golden(root):
    for language in ["english", "chinese"]:
        parent, report = build(root, language, language)
        check(int(report["search.kept"]) == len(GOLDEN["kept"][language]) and report["search.duplicates"] == "2",
              "independent fixed dedup selection")
        check(report["search.no_text"] == "2" and report["recommendation.delivered"] == "7", "no_text/empty TF distinct")
        verify_snapshot(root, parent, language)
        cases = [("lantern", ["lantern"]), ("BLUE blue green", ["blue", "blue", "green"]),
                 ("lantern absent", ["lantern", "absent"]), ("unknown", ["unknown"])]
        if language == "chinese":
            cases += [("纸船 灯笼", ["纸船", "灯笼"]), ("铜壶 blue", ["铜壶", "blue"])]
        for text, tokens in cases:
            for mode in ["and", "or"]:
                for k in [1, 10]:
                    verify_search(parent, language, text, tokens, mode, k, 12)
        for key in ["lantrn", "blue", "z", "纸船", "笼"]:
            verify_suggest(parent, language, key)
        suggestions, _ = verify_suggest(parent, language, "lantrn")
        chosen = suggestions["results"][0]["term"]
        check(chosen == "lantern", "manual choice from visible suggestions")
        verify_search(parent, language, chosen, ["lantern"])
    subset = ["zh-river.txt", "en-river.txt"]
    mixed, _ = build(root, "mixed", "english", "chinese", rec_subset=subset)
    verify_snapshot(root, mixed, "english", "chinese", subset)
    out, _ = verify_suggest(mixed, "chinese", "灯笼", subset)
    chosen = out["results"][0]["term"]
    check(chosen == "灯笼", "independent Chinese recommendation branch")
    check(query(mixed, chosen, "chinese")[0]["results"] == [], "chosen term may be absent in English search branch")


def rejection(root):
    parent, _ = build(root, "valid")
    original = inventory(parent)
    for name in FILES:
        for mutation in ["missing", "truncated", "appended"]:
            copied = root / (name + "-" + mutation)
            shutil.copytree(parent, copied)
            path = copied / "0" / name
            if mutation == "missing":
                path.unlink()
            else:
                data = path.read_bytes()
                path.write_bytes(data[:-1] if mutation == "truncated" else data+b"!")
            before = inventory(copied)
            for operation, text in [("SEARCH", "lantern"), ("SUGGEST", "lantrn")]:
                query(copied, text, operation=operation, code=1)
            check(inventory(copied) == before, "damaged snapshot remains unchanged")
    for kind in ["extra", "directory", "symlink"]:
        copied = root / kind
        shutil.copytree(parent, copied)
        extra = copied / "0" / "unexpected"
        if kind == "directory":
            extra.mkdir()
        elif kind == "symlink":
            extra.symlink_to(parent / "0" / "documents.bin")
        else:
            extra.write_bytes(b"extra")
        query(copied, "lantern", code=1)
        query(copied, "lantrn", operation="SUGGEST", code=1)
    semantic = semantic_copy(parent, root)
    for operation, text in [("SEARCH", "lantern"), ("SUGGEST", "lantrn")]:
        p = query(semantic, text, operation=operation, code=1)
        check(b"reader.semantic" in p.stderr, "valid hashes still reject cross-file character relation")
    for text in ["", "!!!", "a THE is on"]:
        query(parent, text, code=1)
    query(parent, "lantern", version=1, code=1)
    query(parent, "lantern", version="01", code=2)
    query(parent, "lantern", extra=["--mode", "invalid"], code=2)
    query(parent, "lantern", extra=["--max-output-bytes", 1], code=1)
    bad = root / "bad.txt"
    bad.write_bytes(b"invalid\xff")
    target = root / "rejected-publication"
    target.mkdir()
    invoke("indexer", ["--output", target, "--version", 0, "--build-version", "original-v1-golden-1",
                        "--search-txt", bad, "--recommendation-empty"], 1)
    check(list(target.iterdir()) == [], "input rejection occurs before Writer creates publication")
    check(inventory(parent) == original, "all rejection calls preserve original valid snapshot")


def lifecycle(root):
    parent, _ = build(root, "first")
    reverse, _ = build(root, "permuted", reverse=True)
    check(inventory(parent) == inventory(reverse), "same input/path/version reordered: eight files byte identical")
    first, raw = verify_search(parent, "english", "lantern blue", ["lantern", "blue"])
    check(verify_search(reverse, "english", "lantern blue", ["lantern", "blue"])[1] == raw,
          "fresh processes and independent builds produce identical JSON")
    maximum = (1 << 64)-1
    endpoint, _ = build(root, "endpoint", version=maximum)
    for name in FILES[:6]:
        check((parent / "0" / name).read_bytes() == (endpoint / str(maximum) / name).read_bytes(),
              "version does not change payload identities/weights")
    check(query(endpoint, "lantern", version=maximum)[0]["results"], "uint64 maximum actual loaded version")
    semantic = semantic_copy(parent, root)
    before = inventory(root)
    p = invoke("probe", [parent, semantic, DATA / "dictionary.txt", DATA / "hmm.txt"])
    check(b"iterations=32 checks=480" in p.stdout and b"failures=0" in p.stdout, "same-process lifecycle checks executed")
    print(p.stdout.decode().strip())
    with open("/dev/full", "wb") as sink:
        p = invoke("cli", ["SEARCH", "--snapshot", parent, "--version", 0, "--query", "lantern"], 3, stdout=sink)
        check(b"cli.output" in p.stderr, "published snapshot retained after output failure")
    check(inventory(root) == before, "lifecycle/output failures do not alter any owned fixture or snapshot")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ["indexer", "cli", "probe", "fixtures", "data"]:
        parser.add_argument("--"+name, required=True)
    parser.add_argument("--suite", choices=["golden", "rejection", "lifecycle"], required=True)
    OPTIONS = parser.parse_args()
    DATA = Path(OPTIONS.data).resolve()
    GOLDEN = json.loads((DATA / "expected.json").read_text())
    fixture = Path(OPTIONS.fixtures)
    original = inventory(fixture)
    resources_before = inventory(DATA)
    with tempfile.TemporaryDirectory(prefix="siftwing-v1-") as directory:
        root = Path(directory)
        inputs = root / "input"
        inputs.mkdir()
        manifest = json.loads((fixture / "manifest.json").read_text())
        for record in manifest["files"]:
            source = fixture / record["path"]
            check(len(source.read_bytes()) == record["size_bytes"] and sha(source.read_bytes()) == record["sha256"],
                  "original M0.05 local bytes baseline")
            target = inputs / record["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        copied = inventory(inputs)
        {"golden": golden, "rejection": rejection, "lifecycle": lifecycle}[OPTIONS.suite](root)
        check(inventory(inputs) == copied, "all original copied inputs remain unchanged")
    check(inventory(fixture) == original and inventory(DATA) == resources_before, "public original fixture/resources read only")
    print(f"suite={OPTIONS.suite} checks={CHECKS} processes={dict(PROCESSES)} failures=0; V1原创固定黄金与独立参考")
