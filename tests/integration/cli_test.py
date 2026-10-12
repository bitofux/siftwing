#!/usr/bin/env python3
"""真实indexer→CLI原创验收，独立计算排名/Unicode摘要/高亮/推荐期望。

主链路期望来自手写抽取黄金与Python参考，不调用生产函数生成期望。
既有Writer探针只补任意合法稀疏身份/缺失正文/元数据字节边界，显式TF不冒充抽取事实。
所有检查在Release同样运行；每组临时目录独立，不使用私有语料。
"""
import argparse
from collections import Counter
import hashlib
import html
import json
import math
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile

CHECKS = 0
PROCESSES = Counter()


def check(condition, label):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(label)


def invoke(kind, args, expected=0, stdin=None, stdout=subprocess.PIPE):
    PROCESSES[kind] += 1
    result = subprocess.run([getattr(OPTIONS, kind), *[v if isinstance(v, bytes) else str(v) for v in args]], input=stdin,
                            stdout=stdout, stderr=subprocess.PIPE, timeout=20)
    check(result.returncode == expected, (kind, args, result.returncode, result.stdout, result.stderr))
    return result


def command(parent, version, operation, text, extra=()):
    return [operation, "--snapshot", parent, "--version", str(version),
            "--query" if operation == "SEARCH" else "--key", text, *extra]


def response(args):
    result = invoke("cli", args)
    check(result.stderr == b"", "successful CLI has no diagnostic")
    check(result.stdout.endswith(b"\n") and result.stdout.count(b"\n") == 1, "one complete JSON line")
    value = json.loads(result.stdout.decode("utf-8"), parse_constant=lambda s: check(False, s))
    check(value["schema_version"] == 1, "fixed schema")
    check(isinstance(value["snapshot_version"], str), "uint64 version is lossless string")
    parent = Path(args[args.index("--snapshot") + 1])
    version = str(args[args.index("--version") + 1])
    manifest = (parent / version / "manifest.txt").read_text().splitlines()
    check(value["snapshot_version"] == manifest[1].split()[1] and
          value["build_version"] == manifest[1].split()[2], "declared snapshot identity")
    source = manifest[2 if args[0] == "SEARCH" else 3].split()
    check(value["input_sha256"] == source[-2] and value["snapshot_config_sha256"] == source[-1],
          "correct independent branch provenance declaration")
    return value, result.stdout


def write(root, name, value):
    path = root / name
    path.write_bytes(value.encode() if isinstance(value, str) else value)
    return path


def inventory(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file() and not p.is_symlink()}


def build(root, search=(), recommendation=(), version=0, extra=()):
    parent = root / ("snap-" + str(version))
    parent.mkdir()
    args = ["--output", parent, "--version", version, "--build-version", "original-cli-golden-1",
            "--search-dedup-distance", 0]
    for role, sources in [("search", search), ("recommendation", recommendation)]:
        if not sources:
            args += [f"--{role}-empty"]
        for kind, path in sources:
            args += [f"--{role}-{kind}", path]
    invoke("indexer", args + list(extra))
    return parent


def words(value):
    return Counter(re.findall(r"[a-z0-9]+", value.lower()))


def rank(records, tf, mode, k):
    """独立数学向量参考；有限分数近似比较，精确同分黄金另单独断言身份。"""
    all_terms = sorted({word for record in records for word in record["tf"]})
    idf = {word: 1 + math.log((len(records) + 1) /
                             (sum(word in record["tf"] for record in records) + 1)) for word in all_terms}
    known = {word: count * idf[word] for word, count in tf.items() if word in idf}
    if not known or (mode == "and" and any(word not in idf for word in tf)):
        return []
    qnorm = math.sqrt(sum(value * value for value in known.values()))
    output = []
    for record in records:
        matches = [word in record["tf"] for word in tf]
        if not (all(matches) if mode == "and" else any(matches)):
            continue
        vector = {word: count * idf[word] for word, count in record["tf"].items()}
        norm = math.sqrt(sum(value * value for value in vector.values()))
        score = min(1, sum(value * vector.get(word, 0) for word, value in known.items()) / (qnorm * norm))
        output.append((record["id"], score))
    return sorted(output, key=lambda item: (-item[1], item[0]))[:k]


def folded(value):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in value)


def display(body, terms, window, leading):
    normalized = folded(body)
    starts = [normalized.find(folded(word)) for word in terms if folded(word) in normalized]
    width = min(len(body), window)
    start = min(max(0, min(starts) - leading), len(body) - width) if starts else 0
    text = body[start:start+width]
    mask = [False] * len(text)
    for word in terms:
        word = folded(word)
        for i in range(len(text)):
            if folded(text).startswith(word, i):
                mask[i:i+len(word)] = [True] * len(word)
    spans, pieces, i = [], [], 0
    while i < len(text):
        j = i + 1
        while j < len(text) and mask[j] == mask[i]:
            j += 1
        escaped = html.escape(text[i:j], quote=True).replace("&#x27;", "&#39;")
        if mask[i]:
            spans.append({"byte_offset": len(text[:i].encode()), "byte_length": len(text[i:j].encode())})
            pieces.append("<mark>" + escaped + "</mark>")
        else:
            pieces.append(escaped)
        i = j
    return {"text": text, "source_byte_offset": len(body[:start].encode()), "matched": bool(starts),
            "truncated_before": start > 0, "truncated_after": start + width < len(body),
            "html": "".join(pieces), "spans": spans}


def verify_search(parent, records, text, tf=None, mode="or", k=10, window=160, leading=40, extra=()):
    tf = words(text) if tf is None else tf
    flags = ["--mode", mode, "--top-k", k, "--summary-window", window, "--summary-context", leading, *extra]
    output, raw = response(command(parent, 0, "SEARCH", text, flags))
    check(output["command"] == "SEARCH" and output["mode"] == mode and output["top_k"] == k, "search options")
    check(output["terms"] == [{"term": word, "tf": tf[word]} for word in sorted(tf)], "independent query TF")
    expected = rank(records, tf, mode, k)
    check([int(hit["document_id"]) for hit in output["results"]] == [entry[0] for entry in expected], "ranked identities")
    for hit, (identity, score) in zip(output["results"], expected):
        record = next(item for item in records if item["id"] == identity)
        check(math.isclose(hit["score"], score, abs_tol=3e-15, rel_tol=3e-15), "independent cosine")
        check(hit["title"] == record["title"] and hit["url"] == record.get("url"), "same snapshot metadata")
        check(hit["summary"] == display(record["body"], tf, window, leading), "independent scalar window/highlight")
    return output, raw


def distance(a, b):
    """完整二维标量距离矩阵，独立于生产单行DP。"""
    matrix = [[0] * (len(b) + 1) for _ in range(len(a) + 1)]
    for i in range(len(a) + 1):
        matrix[i][0] = i
    for j in range(len(b) + 1):
        matrix[0][j] = j
    for i, left in enumerate(a, 1):
        for j, right in enumerate(b, 1):
            matrix[i][j] = min(matrix[i-1][j] + 1, matrix[i][j-1] + 1,
                               matrix[i-1][j-1] + (left != right))
    return matrix[-1][-1]


def verify_suggest(parent, frequencies, key, k=10, extra=(), prepared=None):
    out, raw = response(command(parent, 0, "SUGGEST", key, ["--top-k", k, *extra]))
    normalized = key if prepared is None else prepared
    check(out["key"] == normalized and out["command"] == "SUGGEST", "independent key contract")
    expected = sorted((distance(normalized, word), -count, word) for word, count in frequencies.items()
                      if set(word) & set(normalized))[:k]
    vocabulary = sorted(frequencies)
    check(out["results"] == [{"term_id": str(vocabulary.index(word)), "term": word,
                              "distance": d, "frequency": -neg} for d, neg, word in expected],
          "independent candidate set/distance/frequency/byte order")
    return out, raw


def custom(root, name, records, recommended=None):
    """显式TF只用于表示合同，不声称该TF由原正文分析得到。"""
    parent = root / name
    parent.mkdir()
    def encoded(value):
        return (value.encode() if isinstance(value, str) else value).hex() or "-"
    def tf(value):
        return str(len(value)) + " " + " ".join(f"{encoded(word)} {count}" for word, count in value.items())
    data = str(len(records)) + "\n"
    for record in records:
        record.setdefault("url", f"https://original.invalid/{record['id']}" if record["id"] % 2 else None)
        data += f"{record['id']} {encoded(record['title'])} {encoded(record['body'])} {tf(record['tf'])}\n"
    recommended = recommended or {}
    data += f"{1 if recommended else 0} {tf(recommended)}\n"
    invoke("writer", [parent, 0], stdin=data.encode())
    return parent


def golden(root):
    a = write(root, "a.txt", "RED blue\r\n<&\"'\\>")
    duplicate = write(root, "b.txt", a.read_bytes())
    rss = write(root, "c.xml", '<rss version="2.0"><channel><item><title>空</title></item><item>'
                '<title>标题&lt;&amp;&quot;</title><link>https://original.invalid/?a=1&amp;b=2</link>'
                '<description><![CDATA[<p>red GREEN</p>]]></description></item></channel></rss>')
    d = write(root, "d.txt", "green blue")
    rec = write(root, "recommendation.txt", "cat cat bat bat bat rat rat rat act dog dog dog")
    parent = build(root, [("txt", d), ("rss", rss), ("txt", duplicate), ("txt", a)], [("txt", rec)])
    records = [{"id": 0, "title": "a", "body": "RED blue\n<&\"'\\>", "tf": words("red blue")},
               {"id": 3, "title": '标题<&"', "body": "red GREEN", "tf": words("red green"),
                "url": "https://original.invalid/?a=1&b=2"},
               {"id": 4, "title": "d", "body": "green blue", "tf": words("green blue")}]
    return parent, records, words(rec.read_text())


def integration(root):
    parent, records, frequencies = golden(root)
    before = inventory(root)
    for text in ["red", "RED red blue", "red blue", "red absent", "unknown", "green", "AND OR"]:
        for mode in ["and", "or"]:
            for k in [1, 10]:
                verify_search(parent, records, text, mode=mode, k=k)
    tie, _ = verify_search(parent, records, "red")
    check([hit["document_id"] for hit in tie["results"]] == ["0", "3"] and
          tie["results"][0]["score"] == tie["results"][1]["score"], "exact tie uses sparse DocId")
    for window, leading in [(1, 0), (4, 0), (7, 2), (4096, 0)]:
        verify_search(parent, records, "red blue", window=window, leading=leading)
    verify_search(parent, records, "red", k=100)
    for key in ["cat", "aa", "CAT", "bat", "t", "z", "rat", "é", "中"]:
        for k in [1, 10]:
            verify_suggest(parent, frequencies, key, k)
    suggestions, raw = verify_suggest(parent, frequencies, " CAT\u3000", extra=["--normalize-key"], prepared="cat")
    verify_suggest(parent, frequencies, " CAT\u3000", extra=["--normalize-key"], prepared="cat")
    chosen = suggestions["results"][0]["term"]
    search, _ = verify_search(parent, records, chosen)
    check(search["results"] == [], "recommended vocabulary independent of search; selected word may not match")
    repeat = response(command(parent, 0, "SUGGEST", " CAT\u3000", ["--normalize-key"]))[1]
    check(raw == repeat, "new process byte deterministic")
    check(inventory(root) == before, "all original sources/resources/snapshot unchanged by CLI")
    stop = write(root, "stopwords", "BLUE\n")
    verify_search(parent, records, "red blue", tf=words("red"), extra=["--stopwords", stop])
    out, _ = response(command(parent, 0, "SEARCH", "red", ["--stopwords", stop]))
    check(out["query_config"]["stopwords"] == hashlib.sha256(stop.read_bytes()).hexdigest(), "actual resource hash reported")
    copied = write(root, "same-stopwords", stop.read_bytes())
    check(response(command(parent, 0, "SEARCH", "red", ["--stopwords", copied]))[1] ==
          response(command(parent, 0, "SEARCH", "red", ["--stopwords", stop]))[1], "same resource content relocated")
    # 手写中文词界，中文与英文两支输入/资源不同，正式indexer而非测试Writer。
    dictionary = write(root, "dictionary", "北京 100 ns\n中国 100 ns\n中文 100 n\n单 1 n\n")
    model = write(root, "model", "# 原创HMM\n0 -100 -100 -100\n-100 0 -100 -100\n0 -100 -100 -100\n"
                  "-100 0 -100 -100\n0 -100 -100 -100\n曙:0,光:0\n曙:0,光:0\n曙:0,光:0\n曙:-100,光:-100\n")
    user = write(root, "user", "中国北京 1000 nz\n")
    cn = write(root, "cn.txt", "中国北京 曙光 RED")
    crss = write(root, "cn.xml", '<rss version="2.0"><channel><item><title>中文</title><description>北京 BLUE</description></item></channel></rss>')
    cnroot = root / "chinese"
    cnroot.mkdir()
    flags = ["--tokenizer", "cppjieba", "--dictionary", dictionary, "--model", model,
             "--user-dictionary", user, "--hmm", "on"]
    indexflags = ["--search-" + str(flag)[2:] if str(flag).startswith("--") else flag for flag in flags]
    cpp = build(cnroot, [("txt", cn), ("rss", crss)], [("txt", root / "recommendation.txt")], extra=indexflags)
    cnrecords = [{"id": 0, "title": "cn", "body": "中国北京 曙光 RED", "tf": Counter({"中国北京": 1, "曙光": 1, "red": 1})},
                 {"id": 1, "title": "中文", "body": "北京 BLUE", "tf": Counter({"北京": 1, "blue": 1})}]
    for text, tf in [("中国北京", Counter({"中国北京": 1})), ("曙光 RED", Counter({"曙光": 1, "red": 1})),
                     ("北京", Counter({"北京": 1})), ("中国北京 北京", Counter({"中国北京": 1, "北京": 1}))]:
        for mode in ["and", "or"]:
            verify_search(cpp, cnrecords, text, tf=tf, mode=mode, window=4, leading=0, extra=flags)
    verify_suggest(cpp, frequencies, "cat")
    # 推荐中文独立累计，黄金标量距离与词ID次序来自Python，不与搜索词表混用。
    cnrecroot = root / "chinese-rec"
    cnrecroot.mkdir()
    recflags = ["--recommendation-" + str(flag)[2:] if str(flag).startswith("--") else flag for flag in flags]
    cnp = build(cnrecroot, [], [("txt", cn), ("rss", crss)], extra=recflags)
    cntf = Counter({"中国北京": 1, "曙光": 1, "red": 1, "北京": 1, "blue": 1})
    for key in ["中国", "京", "曙", "北京", "BLUE"]:
        verify_suggest(cnp, cntf, key)
    # 任意身份、控制字符/元数据、无正文和相邻重叠合并，使用原Writer接口额外验收。
    maximum = (1 << 64) - 1
    special = [{"id": maximum, "title": '标题\n\t\x00"\\', "body": "中国 ababa<&\"'", "tf": Counter({"aba": 1, "ba": 1})},
               {"id": 42, "title": "缺失正文", "body": "", "tf": Counter({"red": 1})}]
    sp = custom(root, "sparse", special, {"aba": 3, "ba": 2})
    verify_search(sp, special, "aba ba", window=160, leading=40)
    verify_search(sp, special, "red")
    verify_search(sp, special, "aba", window=3, leading=0)
    verify_suggest(sp, Counter({"aba": 3, "ba": 2}), "aba")
    rng = random.Random(135)
    for _ in range(15):
        text = " ".join(rng.choices(["red", "blue", "green", "unknown"], k=rng.randint(1, 5)))
        verify_search(parent, records, text, mode=rng.choice(["and", "or"]), k=rng.choice([1, 2, 10]))
    for version in [0, maximum]:
        eroot = root / ("empty-" + str(version))
        eroot.mkdir()
        empty = build(eroot, version=version)
        for operation, key in [("SEARCH", "red"), ("SUGGEST", "cat")]:
            out, _ = response(command(empty, version, operation, key))
            check(out["results"] == [] and out["snapshot_version"] == str(version), "empty complete/version endpoints")


def failures(root):
    parent, records, _ = golden(root)
    base = command(parent, 0, "SEARCH", "red")
    before = inventory(root)
    for args in [[], ["search"], ["--help", "SEARCH"], ["SEARCH"], ["SEARCH", "--snapshot", parent],
                 base + ["--query", "blue"], base + ["--key", "red"], base + ["--normalize-key"],
                 base + ["--mode", "AND"], base + ["--hmm", "off"], base + ["--tokenizer", "invalid"],
                 base + ["--tokenizer", "cppjieba"], base + ["--unknown", "x"], base + ["--extra"],
                 base + ["--summary-window", 1], base + ["--summary-window", 0],
                 base + ["--summary-window", 4097], base + ["--summary-context", 160],
                 command(parent, 0, "SUGGEST", "cat", ["--mode", "or"]),
                 command(parent, 0, "SUGGEST", "cat", ["--stopwords", "missing"]),
                 command(parent, 0, "SUGGEST", "cat", ["--query", "cat"])]:
        check(invoke("cli", args, 2).stdout == b"", "syntax produces no JSON")
    for flag, cap in [("--max-query-bytes", 65536), ("--max-top-k", 100), ("--max-work", 50000000),
                      ("--max-output-bytes", 16777216), ("--max-snapshot-bytes", 536870912),
                      ("--max-resource-bytes", 16777216), ("--top-k", 100)]:
        for value in [0, cap + 1, "01", "-1", "+1", "1x", "18446744073709551616"]:
            check(invoke("cli", base + [flag, value], 2).stdout == b"", "budget syntax rejects")
    for version in ["00", "-1", "+1", "18446744073709551616"]:
        check(invoke("cli", command(parent, version, "SEARCH", "red"), 2).stdout == b"", "canonical version")
    for operation, value in [("SEARCH", ""), ("SEARCH", "!!!"), ("SEARCH", " \t"),
                             ("SEARCH", b"\xff"), ("SUGGEST", ""), ("SUGGEST", "red blue"),
                             ("SUGGEST", "red\u3000blue"), ("SUGGEST", b"\xc0\xaf")]:
        check(invoke("cli", command(parent, 0, operation, value), 1).stdout == b"", "business input failure")
    for key in [" \u3000", "red blue"]:
        check(invoke("cli", command(parent, 0, "SUGGEST", key, ["--normalize-key"]), 1).stdout == b"", "normalized key not a search query")
    for extra, context in [(["--max-query-bytes", 2], b"normalize.input_bytes"),
                           (["--max-work", 1], b"ranking.work"),
                           (["--max-output-bytes", 1], b"cli.output_bytes"),
                           (["--max-snapshot-bytes", 1], b"manifest.file_bytes"),
                           (["--top-k", 1, "--max-work", 3], b"cli.display_work")]:
        result = invoke("cli", base + extra, 1)
        check(result.stdout == b"" and context in result.stderr, ("budget actual stage", extra, result.stderr))
    result = invoke("cli", command(parent, 0, "SUGGEST", "cat", ["--max-work", 1]), 1)
    check(b"recommend.work" in result.stderr and result.stdout == b"", "small k does not bypass recommendation work")
    # 有效输出预算恰好端点；再少一字节失败，最终换行计入。
    raw = response(base)[1]
    check(response(base + ["--max-output-bytes", len(raw)])[1] == raw, "JSON exact byte budget")
    check(invoke("cli", base + ["--max-output-bytes", len(raw)-1], 1).stdout == b"", "JSON one byte below")
    check(response(base + ["--max-query-bytes", 3])[1] == raw, "query bytes inclusive endpoint")
    display_work = sum(len(record["body"]) for record in records if "red" in record["tf"]) * 3
    check(response(base + ["--max-work", display_work])[1] == raw, "aggregate display work inclusive endpoint")
    result = invoke("cli", base + ["--max-work", display_work-1], 1)
    check(result.stdout == b"" and b"cli.display_work" in result.stderr, "all selected bodies counted")
    stop = write(root, "stops", "red\n")
    check(invoke("cli", base + ["--stopwords", stop], 1).stdout == b"", "all stopwords query.empty not successful empty")
    bad = write(root, "bad-stops", b"\xff")
    link = root / "resource-link"
    link.symlink_to(stop)
    fifo = root / "fifo"
    import os
    os.mkfifo(fifo)
    for source in [root / "missing", root, link, fifo, root / ".." / root.name / "stops", bad]:
        result = invoke("cli", base + ["--stopwords", source], 1)
        check(result.stdout == b"", "resources reject safely")
    check(invoke("cli", base + ["--stopwords", stop, "--max-resource-bytes", 1], 1).stdout == b"", "preallocation resource budget")
    dictionary = write(root, "dictionary", "中国 100 n\n")
    model = write(root, "hmm", "# 原创模型\n0 -100 -100 -100\n-100 0 -100 -100\n0 -100 -100 -100\n"
                  "-100 0 -100 -100\n0 -100 -100 -100\n中:0,国:0\n中:0,国:0\n中:0,国:0\n中:-100,国:-100\n")
    malformed = write(root, "bad-chinese-resource", "invalid resource\n")
    malformed_user = write(root, "bad-user-dictionary", "bad 0 n\n")
    for dictfile, hmmfile, userfile in [(malformed, model, None), (dictionary, malformed, None),
                                      (dictionary, root / "missing", None), (link, model, None),
                                      (dictionary, model, malformed_user)]:
        flags = ["--tokenizer", "cppjieba", "--dictionary", dictfile, "--model", hmmfile]
        if userfile:
            flags += ["--user-dictionary", userfile]
        check(invoke("cli", base + flags, 1).stdout == b"", "actual Chinese resource initialization rejects")
    symlink_parent = root / "parent-link"
    symlink_parent.symlink_to(parent, target_is_directory=True)
    for source, version in [(root / "missing", 0), (symlink_parent, 0), (parent, 999),
                            (parent / ".." / parent.name, 0)]:
        check(invoke("cli", command(source, version, "SEARCH", "red"), 1).stdout == b"", "Reader path/version reject")
    for artifact in ["COMPLETE", "manifest.txt", "documents.bin", "search-weights.txt", "recommendation-dictionary.txt"]:
        copied = root / ("bad-" + artifact)
        shutil.copytree(parent, copied)
        path = copied / "0" / artifact
        # 固定叶子名来自快照合同；不根据Reader返回生成期望。
        path.write_bytes(path.read_bytes() + b"!")
        broken_before = inventory(copied)
        for operation, key in [("SEARCH", "red"), ("SUGGEST", "cat")]:
            check(invoke("cli", command(copied, 0, operation, key), 1).stdout == b"", "both commands require complete snapshot")
        check(inventory(copied) == broken_before, "corrupt input not repaired")
    invalid = custom(root, "invalid-metadata", [{"id": 0, "title": b"\xff", "body": "red", "tf": words("red")}])
    result = invoke("cli", command(invalid, 0, "SEARCH", "red"), 1)
    check(result.stdout == b"" and b"cli.output_utf8" in result.stderr, "Reader raw metadata is not silently replaced")
    check(response(command(invalid, 0, "SUGGEST", "cat"))[0]["results"] == [], "recommendation does not display unrelated raw metadata")
    invalid_body = custom(root, "invalid-body", [{"id": 0, "title": "合法标题", "body": b"\xff", "tf": words("red")}])
    result = invoke("cli", command(invalid_body, 0, "SEARCH", "red"), 1)
    check(result.stdout == b"" and b"cli.display_utf8" in result.stderr, "display validates full body")
    with open("/dev/full", "wb") as sink:
        result = invoke("cli", base, 3, stdout=sink)
        check(b"cli.output" in result.stderr, "write failure status")
    readfd, writefd = os.pipe()
    os.close(readfd)
    with os.fdopen(writefd, "wb") as sink:
        result = invoke("cli", base, 3, stdout=sink)
        check(b"cli.output" in result.stderr, "SIGPIPE becomes stream error")
    help_result = invoke("cli", ["--help"])
    check(b"SEARCH" in help_result.stdout and b"SUGGEST" in help_result.stdout, "formal help")
    # 最初所有源/快照仍与原清单相同；新增故障夹具不混入原清单。
    now = inventory(root)
    check(all(now[name] == sha for name, sha in before.items()), "all original snapshot/source bytes unchanged")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--indexer", required=True)
    parser.add_argument("--writer", required=True)
    parser.add_argument("--suite", choices=["integration", "failures"], required=True)
    OPTIONS = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="siftwing-cli-") as directory:
        (integration if OPTIONS.suite == "integration" else failures)(Path(directory))
    print(f"checks={CHECKS} processes={dict(PROCESSES)} failures=0; suite={OPTIONS.suite}; 原创黄金与独立参考")
