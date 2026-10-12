#!/usr/bin/env python3
"""正式indexer进程的原创端到端验收；Python独立计算TF、去重、权重及来源摘要。

不调用生产分词/SimHash/Writer产生期望。Reader探针只负责证明完整产物可加载；
快照字段、CRC、TF/DF/IDF/L2、字符关系仍由本文件独立核对，Release同样执行。
"""
import argparse
from collections import Counter
import hashlib
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile
import zlib

CHECKS = 0
PROCESSES = 0
ARTIFACTS = ["documents.bin", "offsets.bin", "search-lexicon.txt", "search-weights.txt",
             "recommendation-dictionary.txt", "recommendation-characters.txt"]


def check(value, message):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def invoke(args, expected=0, stdout=subprocess.PIPE):
    global PROCESSES
    PROCESSES += 1
    result = subprocess.run([OPTIONS.indexer, *map(str, args)], stdout=stdout, stderr=subprocess.PIPE,
                            text=True, timeout=20)
    check(result.returncode == expected, (args, result.returncode, result.stdout, result.stderr))
    return result


def arguments(output, search=(), recommendation=(), version=0, extra=()):
    result = ["--output", output, "--version", str(version), "--build-version", "original-indexer-test-1"]
    for role, sources in [("search", search), ("recommendation", recommendation)]:
        if not sources:
            result += [f"--{role}-empty"]
        for kind, path in sources:
            result += [f"--{role}-{kind}", path]
    return result + list(extra)


def write(root, name, content):
    path = root / name
    path.write_bytes(content.encode() if isinstance(content, str) else content)
    return path


def decode_records(data):
    records = []
    while data:
        magic, version, header, length, identity, ordinal, kind, flags, reserved, crc = struct.unpack(">4sHHQQQBBHI", data[:40])
        check((magic, version, header, reserved) == (b"SWDR", 1, 40, 0), "independent record header")
        current, data = data[:length], data[length:]
        check(zlib.crc32(current[:36] + current[40:]) == crc, "independent record CRC")
        fields, position = [], 40
        for _ in range(6):
            size = struct.unpack(">Q", current[position:position+8])[0]
            position += 8
            fields.append(current[position:position+size])
            position += size
        check(position == length, "complete independent record fields")
        records.append((identity, ordinal, kind, flags, fields))
    return records


def fingerprint(tf):
    if not tf:
        return None
    mask = (1 << 64) - 1
    features = []
    for term, count in tf.items():
        value = 14695981039346656037
        for byte in term.encode():
            value = ((value ^ byte) * 1099511628211) & mask
        for constant in [0xff51afd7ed558ccd, 0xc4ceb9fe1a85ec53]:
            value = ((value ^ (value >> 33)) * constant) & mask
        value ^= value >> 33
        features.append((value, count))
    return sum(1 << bit for bit in range(64)
               if sum(count if value & (1 << bit) else -count for value, count in features) > 0)


def selected_records(sources, bodies, stops=(), tokens=None):
    """bodies是原创抽取黄金；None表示no_text，仍保留原item编号占位。"""
    result, identity = [], 0
    for kind, path in sorted(sources, key=lambda source: str(source[1]).encode()):
        entries = bodies[path]
        if not entries:
            identity += 1
        for ordinal, (title, body) in enumerate(entries):
            if body is not None:
                words = tokens[body] if tokens is not None else re.findall(r"[a-z0-9]+", body.lower())
                tf = Counter(word for word in words if word not in stops)
                result.append((identity, path, ordinal, kind, title, body, tf))
            identity += 1
    return result


def expected_kept(records, threshold):
    kept, duplicate, empty, comparisons = [], 0, 0, 0
    for record in records:
        candidate = fingerprint(record[-1])
        if candidate is None:
            empty += 1
            continue
        for prior in kept:
            comparisons += 1
            if bin(candidate ^ fingerprint(prior[-1])).count("1") <= threshold:
                duplicate += 1
                break
        else:
            kept.append(record)
    return kept, duplicate, empty, comparisons


def input_digest(sources):
    description = "SIFTWING-INDEXER-INPUTS 1\n"
    for kind, path in sorted(sources, key=lambda source: str(source[1]).encode()):
        raw = path.read_bytes()
        description += f"F {kind} {str(path)[1:].encode().hex()} {len(raw)} {sha(raw)}\n"
    return sha(description.encode())


def verify(parent, version, report, search_sources, rec_sources, search_records, rec_records, threshold):
    directory = parent / str(version)
    files = {p.name: p.read_bytes() for p in directory.iterdir()}
    check(set(files) == set(ARTIFACTS + ["manifest.txt", "COMPLETE"]), "exact eight regular files")
    check(all(not p.is_symlink() and p.is_file() for p in directory.iterdir()), "regular snapshot files")
    lines = files["manifest.txt"].decode().splitlines()
    check(len(lines) == 10 and lines[0] == "SIFTWING-SNAPSHOT 1", "canonical manifest")
    check(lines[1] == f"I {version} original-indexer-test-1", "explicit build/version identity")
    for line, name in zip(lines[4:], ARTIFACTS):
        check(line == f"F {name} 1 {len(files[name])} {sha(files[name])}", "independent payload digest")
    check(sha(files["manifest.txt"]) in files["COMPLETE"].decode(), "completion binds manifest")
    values = dict(line.split("=", 1) for line in report.splitlines())
    check(values["durability"] == "synced" and values["system_errno"] == "0", "published synced")
    for role, sources, line in [("search", search_sources, lines[2]), ("recommendation", rec_sources, lines[3])]:
        check(values[role + ".files"] == str(len(sources)), "explicit file count")
        check(line.split()[-2] == input_digest(sources) == values[role + ".input_sha256"], "actual original bytes/source digest")
        check(re.fullmatch("[0-9a-f]{64}", values[role + ".config_sha256"]) is not None, "config digest")
        check(line.split()[-1] == values[role + ".config_sha256"], "config declaration/report agrees")
    kept, duplicates, empty, comparisons = expected_kept(search_records, threshold)
    for name, expected in [("delivered", len(search_records)), ("kept", len(kept)), ("documents", len(kept)),
                           ("duplicates", duplicates), ("no_features", empty), ("comparisons", comparisons)]:
        check(values["search." + name] == str(expected), "independent search disposition/identity")
    decoded = decode_records(files["documents.bin"])
    check(len(decoded) == len(kept), "only kept records persisted")
    for actual, expected in zip(decoded, kept):
        identity, path, ordinal, kind, title, body, _ = expected
        check(actual[:3] == (identity, ordinal, 1 if kind == "txt" else 2), "original logical IDs/ordinals/kind")
        check(actual[4][:3] == [title.encode(), body.encode(), str(path)[1:].encode()], "original extracted fields")
    offsets = files["offsets.bin"]
    check(len(offsets) == 32 + 24*len(kept), "offset count/length")
    check(struct.unpack(">Q", offsets[16:24])[0] == len(files["documents.bin"]), "offset coverage length")
    check(zlib.crc32(offsets[:24] + offsets[28:]) == struct.unpack(">I", offsets[24:28])[0], "offset CRC")
    position = 0
    for i, record in enumerate(decoded):
        entry = struct.unpack(">QQQ", offsets[32+24*i:56+24*i])
        check(entry[:2] == (record[0], position), "offset exact identity/coverage")
        position += entry[2]
    check(position == len(files["documents.bin"]), "complete offset cover")
    terms = sorted({word for record in kept for word in record[-1]}, key=lambda s: s.encode())
    total = sum(sum(record[-1].values()) for record in kept)
    postings = sum(len(record[-1]) for record in kept)
    check(lines[2].split()[1:5] == list(map(str, [len(kept), len(terms), total, postings])), "independent search statistics")
    lexicon = "SIFTWING-LEXICON 1\nS %d\n" % len(terms) + "".join(
        f"T {i} {term.encode().hex()}\n" for i, term in enumerate(terms))
    check(files["search-lexicon.txt"] == lexicon.encode(), "byte ordered lexicon")
    idf = {word: 1 + math.log((len(kept)+1)/(sum(word in r[-1] for r in kept)+1)) for word in terms}
    norms = {r[0]: math.sqrt(sum((count*idf[word])**2 for word, count in r[-1].items())) for r in kept}
    by_id = {r[0]: r[-1] for r in kept}
    weights = files["search-weights.txt"].decode().splitlines()
    check(weights[:2] == ["SIFTWING-WEIGHTS 1 1", f"S {len(kept)} {total} {postings}"], "weight header/counts")
    seen_docs, seen_terms, seen_postings = set(), set(), set()
    for line in weights[2:]:
        fields = line.split()
        if fields[0] == "D":
            identity, tokens_count, distinct = map(int, fields[1:4])
            check((tokens_count, distinct) == (sum(by_id[identity].values()), len(by_id[identity])), "TF document statistics")
            value = struct.unpack(">d", bytes.fromhex(fields[4]))[0]
            check(math.isclose(value, norms[identity], rel_tol=1e-12), "independent document L2")
            seen_docs.add(identity)
        elif fields[0] == "T":
            term_id, word, df = int(fields[1]), bytes.fromhex(fields[2]).decode(), int(fields[3])
            check(word == terms[term_id] and df == sum(word in r[-1] for r in kept), "independent DF/term")
            check(math.isclose(struct.unpack(">d", bytes.fromhex(fields[4]))[0], idf[word], rel_tol=1e-12), "independent IDF")
            seen_terms.add(term_id)
        else:
            check(fields[0] == "P", "only canonical weight fields")
            term_id, identity, count = map(int, fields[1:4])
            word = terms[term_id]
            check(by_id[identity][word] == count, "original per-document TF")
            check(math.isclose(struct.unpack(">d", bytes.fromhex(fields[4]))[0], count*idf[word]/norms[identity], rel_tol=1e-12), "independent normalized weight")
            seen_postings.add((term_id, identity))
    check(len(seen_docs) == len(kept) and len(seen_terms) == len(terms) and len(seen_postings) == postings, "complete weights")
    accumulated = Counter()
    for record in rec_records:
        accumulated.update(record[-1])
    rec_terms = sorted(accumulated, key=lambda s: s.encode())
    rec_total = sum(accumulated.values())
    rec = "SIFTWING-RECOMMENDATION 1\nS %d %d %d\n" % (len(rec_records), rec_total, len(rec_terms))
    rec += "".join(f"T {i} {term.encode().hex()} {accumulated[term]}\n" for i, term in enumerate(rec_terms))
    check(files["recommendation-dictionary.txt"] == rec.encode(), "independent deliveries and occurrence counts")
    check(values["recommendation.documents"] == values["recommendation.delivered"] == str(len(rec_records)), "recommendation independent of search")
    characters = {}
    for i, term in enumerate(rec_terms):
        for char in set(term):
            characters.setdefault(ord(char), []).append(i)
    links = sum(map(len, characters.values()))
    chars = "SIFTWING-CHARACTERS 1\nS %d %d\n" % (len(characters), links)
    chars += "".join(f"C {cp} {' '.join(map(str, ids))}\n" for cp, ids in sorted(characters.items()))
    check(files["recommendation-characters.txt"] == chars.encode(), "independent character set relations")
    check(lines[3].split()[1:6] == list(map(str, [len(rec_records), len(rec_terms), rec_total, len(characters), links])), "independent recommendation manifest")
    loaded = subprocess.run([OPTIONS.reader, str(parent), str(version)], capture_output=True, text=True, timeout=20)
    check(loaded.returncode == 0 and loaded.stdout.startswith("OK\n"), "real SnapshotReader loads both branches")
    check("M " + files["manifest.txt"].hex() in loaded.stdout and "W " + files["search-weights.txt"].hex() in loaded.stdout, "loaded original canonical fields")
    return values, files


def integration(root):
    stops = write(root, "stopwords", "THE\r\n")
    a = write(root, "a.txt", "\ufeffTHE RED red BLUE\r\n")
    b = write(root, "b.txt", "THE RED red BLUE\n")
    xml = write(root, "c.xml", "<rss version='2.0'><channel><?original test?><item><title>原创RSS</title>"
                "<description><![CDATA[<p>blue GREEN</p>]]></description></item>"
                "<item><description> </description></item><item xmlns='urn:ignored'><description>ignored</description></item></channel></rss>")
    d = write(root, "d.txt", "THE")
    e = write(root, "e.txt", " \t\r\n")
    f = write(root, "f.txt", "orange sky")
    r = write(root, "r.txt", "banana RED")
    bodies = {a: [("a", "THE RED red BLUE\n")], b: [("b", "THE RED red BLUE\n")],
              xml: [("原创RSS", "blue GREEN"), ("", None)], d: [("d", "THE")],
              e: [("", None)], f: [("f", "orange sky")], r: [("r", "banana RED")]}
    search = [("txt", path) for path in [a, b, d, e, f]] + [("rss", xml)]
    rec = [("txt", path) for path in [a, b, d, r]] + [("rss", xml)]
    sr, rr = selected_records(search, bodies, {"the"}), selected_records(rec, bodies, {"the"})
    extra = ["--search-stopwords", stops, "--recommendation-stopwords", stops, "--search-dedup-distance", "0"]
    original = {p: sha(p.read_bytes()) for p in root.iterdir() if p.is_file()}
    first, second = root / "out1", root / "out2"
    first.mkdir(); second.mkdir()
    report = invoke(arguments(first, search, rec, extra=extra)).stdout
    values, files = verify(first, 0, report, search, rec, sr, rr, 0)
    check(values["search.duplicates"] == "1" and values["search.no_features"] == "1" and values["search.no_text"] == "2", "distinct duplicate/empty/no_text")
    check(values["search.config_sha256"] != values["recommendation.config_sha256"], "independent branch config descriptions")
    report2 = invoke(arguments(second, list(reversed(search)), list(reversed(rec)), extra=extra)).stdout
    _, files2 = verify(second, 0, report2, search, rec, sr, rr, 0)
    check(files == files2 and report == report2, "same inputs/parameter permutation deterministic all eight files")
    for threshold in [3, 64]:
        target = root / f"threshold-{threshold}"; target.mkdir()
        changed = extra[:-2] + ["--search-dedup-distance", str(threshold)]
        result = invoke(arguments(target, search, rec, extra=changed))
        v, payload = verify(target, 0, result.stdout, search, rec, sr, rr, threshold)
        check(payload["recommendation-dictionary.txt"] == files["recommendation-dictionary.txt"], "search policy does not dedup recommendation")
        check(v["search.config_sha256"] != values["search.config_sha256"], "dedup policy participates config hash")
        check(v["recommendation.config_sha256"] == values["recommendation.config_sha256"], "search config isolated")
    for role in ["both", "search", "recommendation"]:
        target = root / ("empty-"+role); target.mkdir()
        ss = [] if role in ["both", "search"] else search
        rs = [] if role in ["both", "recommendation"] else rec
        result = invoke(arguments(target, ss, rs, version=(1 << 64)-1, extra=extra))
        verify(target, (1 << 64)-1, result.stdout, ss, rs, sr if ss else [], rr if rs else [], 0)
    # 中文词典/HMM均原创；黄金词界手写，不借用cppjieba预测期望。
    dictionary = write(root, "dictionary", "北京 100 ns\n中国 100 ns\n中文 100 n\n单 1 n\n")
    model = write(root, "model", "# 原创HMM\n0 -100 -100 -100\n-100 0 -100 -100\n0 -100 -100 -100\n"
                  "-100 0 -100 -100\n0 -100 -100 -100\n曙:0,光:0\n曙:0,光:0\n曙:0,光:0\n曙:-100,光:-100\n")
    user = write(root, "user", "中国北京 1000 nz\n")
    cn = write(root, "cn.txt", "中国北京 曙光 BLUE")
    cb = {cn: [("cn", "中国北京 曙光 BLUE")]}
    base = ["--recommendation-tokenizer", "cppjieba", "--recommendation-dictionary", dictionary,
            "--recommendation-model", model, "--recommendation-hmm", "on"]
    for with_user in [False, True]:
        target = root / ("cn-user" if with_user else "cn"); target.mkdir()
        flags = base + (["--recommendation-user-dictionary", user] if with_user else [])
        result = invoke(arguments(target, [("txt", cn)], [("txt", cn)], extra=flags))
        reference = ["中国北京", "曙光", "blue"] if with_user else ["中国", "北京", "曙光", "blue"]
        verify(target, 0, result.stdout, [("txt", cn)], [("txt", cn)], selected_records([("txt", cn)], cb),
               selected_records([("txt", cn)], cb, tokens={cb[cn][0][1]: reference}), 3)
    # 两支中文TXT/RSS都走正式命令，词界为原创词典可手算黄金；搜索HMM与推荐独立。
    search_cn = write(root,"search-cn.txt","中国 北京 RED")
    rss_cn = write(root,"rss-cn.xml","<rss version='2.0'><channel><item><title>中文RSS</title>"
                   "<description><![CDATA[<p>中文 BLUE</p>]]></description></item></channel></rss>")
    gold = {search_cn:[("search-cn","中国 北京 RED")],rss_cn:[("中文RSS","中文 BLUE")],**cb}
    words = {"中国 北京 RED":["中国","北京","red"],"中文 BLUE":["中文","blue"],
             "中国北京 曙光 BLUE":["中国","北京","曙光","blue"]}
    ss, rs = [("txt",search_cn),("rss",rss_cn)], [("txt",cn),("rss",rss_cn)]
    target = root/"both-chinese"; target.mkdir()
    flags = base+["--search-tokenizer","cppjieba","--search-dictionary",dictionary,
                  "--search-model",model,"--search-hmm","off"]
    result = invoke(arguments(target,ss,rs,extra=flags))
    verify(target,0,result.stdout,ss,rs,selected_records(ss,gold,tokens=words),
           selected_records(rs,gold,tokens=words),3)
    # 同内容资源改路径不改变配置摘要；源输入换路径则改变input摘要/来源。
    relocated = write(root, "relocated-dictionary", dictionary.read_bytes())
    target = root / "relocated"; target.mkdir()
    flags = [relocated if item == dictionary else item for item in base]
    result = invoke(arguments(target, [("txt", cn)], [("txt", cn)], extra=flags))
    check((target/"0"/"manifest.txt").read_bytes() == (root/"cn"/"0"/"manifest.txt").read_bytes(), "resource relocation preserves config and outputs")
    # 两支选同一篇但停用集合独立，推荐不能复用搜索的TF或保留集合。
    target = root/"independent-stops"; target.mkdir()
    allstop = write(root,"allstop","the\nred\nblue\n")
    result = invoke(arguments(target,[("txt",a)],[("txt",a)],extra=["--search-stopwords",allstop]))
    verify(target,0,result.stdout,[("txt",a)],[("txt",a)],selected_records([("txt",a)],bodies,{"the","red","blue"}),
           selected_records([("txt",a)],bodies),3)
    moved_input = write(root,"moved.txt", a.read_bytes())
    target = root/"moved-input"; target.mkdir()
    result = invoke(arguments(target,[("txt",moved_input)],[("txt",a)]))
    v, _ = verify(target,0,result.stdout,[("txt",moved_input)],[("txt",a)],
                  selected_records([("txt",moved_input)],{moved_input:[("moved",bodies[a][0][1])]}),selected_records([("txt",a)],bodies),3)
    check(v["search.input_sha256"] != v["recommendation.input_sha256"], "input relocation changes actual source digest")
    # 多组原创随机英文TF用独立整数指纹及数学参考交叉检验默认去重策略。
    rng = random.Random(134)
    for case in range(8):
        sources, gold = [], {}
        for i in range(5):
            text = " ".join(rng.choices(["red", "blue", "green", "sky", "orange"], k=1+rng.randrange(10)))
            path = write(root, f"random-{case}-{i}.txt", text)
            sources.append(("txt", path)); gold[path] = [(path.stem, text)]
        target = root / f"random-out-{case}"; target.mkdir()
        result = invoke(arguments(target, sources, sources))
        records = selected_records(sources, gold)
        verify(target, 0, result.stdout, sources, sources, records, records, 3)
    check(all(sha(path.read_bytes()) == digest for path, digest in original.items()), "all original inputs/stops unchanged")


def failures(root):
    output = root / "out"; output.mkdir()
    good = write(root, "good.txt", "red green")
    simple = arguments(output)
    check(invoke(["--help"]).stdout.startswith("用法"), "help succeeds without IO")
    # 帮助也须等待实际写出；缓冲区暂未报错不能作为输出成功证据。
    with open("/dev/full", "w") as full:
        invoke(["--help"], 4, stdout=full)
    for args in [[], ["--help", "extra"], simple+["--unknown", "1"], simple+["--version", "1"],
                 simple+["--search-txt", good], simple+["--search-hmm", "off"],
                 [v if v != "0" else "00" for v in simple],
                 [v if v != "0" else "18446744073709551616" for v in simple],
                 simple+["--search-tokenizer", "cppjieba"], simple+["--search-tokenizer", "invalid"],
                 simple+["--search-hmm", "invalid"], simple+["--search-stopwords", ""],
                 simple+["--search-dedup-distance", "65"], simple+["--output"],
                 ["--output", output, "--version", "0", "--build-version", "bad identity", "--search-empty", "--recommendation-empty"]]:
        result = invoke(args, 2); check(result.stdout == "", "argument failure no success report")
    for flag, above in [("max-input-bytes", 16*1024*1024+1), ("max-total-input-bytes",128*1024*1024+1),
                        ("max-records",10001), ("max-total-text-bytes",128*1024*1024+1), ("max-dedup-comparisons",50000001)]:
        for value in ["0", "-1", "+1", "01", str(above)]:
            invoke(simple+["--"+flag,value],2)
    broken = write(root, "broken.txt", b"red\xff")
    nul = write(root, "nul.txt", b"red\0green")
    malformed = write(root, "malformed.xml", "<rss><channel>")
    dtd = write(root, "dtd.xml", "<!DOCTYPE rss><rss version='2.0'><channel/></rss>")
    atom = write(root, "atom.xml", "<feed/>")
    partial = write(root, "partial.xml", "<rss version='2.0'><channel><item><description><p>red</p></description></item></channel></rss>")
    mixed = write(root, "mixed.xml", "<rss version='2.0'><channel><item><description>red</description></item><item><description><p>bad</p></description></item></channel></rss>")
    many = write(root, "many.xml", "<rss version='2.0'><channel><item><description>red</description></item><item><description>blue</description></item></channel></rss>")
    badstop = write(root, "bad-stopwords", "two words")
    link = root / "link.txt"; link.symlink_to(good)
    linkdir = root / "linkdir"; linkdir.symlink_to(root, target_is_directory=True)
    fifo = root / "fifo"
    import os
    os.mkfifo(fifo)
    scenarios = [([("txt", p)], []) for p in [broken,nul,link,linkdir/"good.txt",fifo,root,root/"missing",root/".."/"x"]]
    scenarios += [([("rss", p)], []) for p in [malformed,dtd,atom,partial,mixed]]
    scenarios += [([("txt",good),("rss",good)], []), ([("txt",good),("txt",root/"."/"good.txt")], []),
                  ([("txt",good)], ["--max-input-bytes","2"]),
                  ([("txt",good)], ["--max-total-input-bytes","2"]),
                  ([("txt",good)], ["--max-total-text-bytes","2"]),
                  ([("rss",many)], ["--max-records","1"]),
                  ([("txt",good)], ["--search-stopwords",badstop])]
    for sources, flags in scenarios:
        result = invoke(arguments(output,sources,extra=flags),1)
        check(result.stdout == "" and not (output/"0").exists(), "failed batch never publishes partial snapshot")
    dictionary = write(root,"dictionary","中国 100 n\n")
    badmodel = write(root,"badmodel","invalid model\n")
    for dictfile, model in [(good,good),(dictionary,badmodel),(root/"missing",good),(link,good)]:
        result = invoke(arguments(output,[("txt",good)],extra=["--search-tokenizer","cppjieba",
                         "--search-dictionary",dictfile,"--search-model",model]),1)
        check(result.stdout == "" and not (output/"0").exists(), "resource initialization never publishes partial state")
    for parent in [root/"nonexistent", good, linkdir]:
        result = invoke(arguments(parent),1); check(result.stdout == "", "invalid output parent")
    # 精确累计输入/标题+正文边界；两个相异指纹使有限实际比较预算被耗尽。
    first = write(root, "a.txt", "red")
    second = write(root, "b.txt", "blue")
    third = write(root, "c.txt", "green")
    invoke(arguments(output,[("txt",first),("txt",second),("txt",third)],
                     extra=["--search-dedup-distance","0","--max-dedup-comparisons","1"]),1)
    invoke(arguments(output,[("txt",first),("txt",second)],extra=["--max-total-input-bytes","6"]),1)
    invoke(arguments(output,[("txt",first),("txt",second)],extra=["--max-records","1"]),1)
    # Reader明确no_text与空篇：空XML、无item、空TXT可成功；没有自动产生可索引文档。
    empty = write(root, "empty.txt", "")
    noitems = write(root, "noitems.xml", "<rss version='2.0'><channel/></rss>")
    result = invoke(arguments(output,[("txt",empty),("rss",noitems)],[("txt",empty)]))
    values = dict(line.split("=",1) for line in result.stdout.splitlines())
    check(values["search.no_text"] == "2" and values["search.documents"] == values["recommendation.documents"] == "0", "no_text not delivered")
    original = {p.name:p.read_bytes() for p in (output/"0").iterdir()}
    invoke(arguments(output),1)
    check(original == {p.name:p.read_bytes() for p in (output/"0").iterdir()}, "existing version unchanged")
    # 输出报告故障发生在发布之后，必须保留完整正式目录并明确已发布。
    published = root/"published"; published.mkdir()
    with open("/dev/full","w") as full:
        result = invoke(arguments(published),4,stdout=full)
    check((published/"0"/"COMPLETE").exists() and "已发布" in result.stderr, "published report failure retained")
    path = root/"boundary"; path.mkdir()
    invoke(arguments(path,[("txt",first)],extra=["--max-input-bytes","3","--max-total-input-bytes","3",
                                              "--max-total-text-bytes","4","--max-records","1"]))
    check((path/"0").is_dir(), "inclusive exact input/record/text budgets")
    # 完整接纳预算用尽后仍有额外no_text来源也不能静默不处理。
    path = root/"remaining"; path.mkdir()
    invoke(arguments(path,[("txt",first),("txt",empty)],extra=["--max-records","1"]),1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--indexer",required=True)
    parser.add_argument("--reader",required=True)
    parser.add_argument("--suite",choices=["integration","failures"],required=True)
    OPTIONS = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="siftwing-indexer-") as directory:
        (integration if OPTIONS.suite == "integration" else failures)(Path(directory))
    print(f"checks={CHECKS} processes={PROCESSES} failures=0; suite={OPTIONS.suite}; 原创黄金与独立参考")
