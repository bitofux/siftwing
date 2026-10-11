"""真实Writer→Reader→Writer，复用独立字节/数学参考，区分固定元数据与版本变化。"""
import argparse
import hashlib
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile

import reader_reference_test as reader_reference
import writer_reference_test as reference

CHECKS = 0
NAMES = reference.NAMES + ["manifest.txt", "COMPLETE"]


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def run(command, expected=0, wire=None):
    result = subprocess.run(command, input=wire, text=True, capture_output=True, timeout=30)
    check(result.returncode == expected,
          f"command={command}; exit={result.returncode}; stdout={result.stdout}; stderr={result.stderr}")
    return result.stdout


def files(folder):
    check(sorted(p.name for p in folder.iterdir()) == sorted(NAMES), "exact eight regular artifacts")
    check(all((folder / name).is_file() and not (folder / name).is_symlink() for name in NAMES),
          "all artifacts are regular files")
    return {name: (folder / name).read_bytes() for name in NAMES}


def loaded(reader, parent, version, expected, records, deliveries, frequencies):
    lines = run([reader, str(parent), str(version)]).splitlines()
    # 记录参考由Python独立编码，推荐字符关系由Unicode标量集合构造，不借用生产codec。
    wanted = ["OK", "M " + expected["manifest.txt"].hex(), "W " + expected["search-weights.txt"].hex()]
    wanted += [f"D {identity} {encoded.hex()}" for identity, encoded in sorted(records)]
    terms = sorted(frequencies)
    wanted += [f"R {deliveries} {sum(frequencies.values())} {len(terms)}"]
    wanted += [f"T {i} {word.hex()} {frequencies[word]}" for i, word in enumerate(terms)]
    characters = {}
    for term_id, word in enumerate(terms):
        for scalar in set(map(ord, word.decode("utf-8"))):
            characters.setdefault(scalar, []).append(term_id)
    wanted += ["C " + str(scalar) + " " + " ".join(map(str, characters[scalar]))
               for scalar in sorted(characters)]
    check(lines == wanted, "complete Reader semantics match independent record/TF/Unicode reference")


def pipeline(args, root):
    parent = root / "pipeline"
    parent.mkdir()
    result = run([args.pipeline, str(parent)])
    print("pipeline " + result.strip())
    first = files(parent / "pipeline-0" / "42")
    check(files(parent / "pipeline-1" / "42") == first, "fresh ingestion/permuted batch produces all eight identical files")
    records = [(0, reader_reference.record(0, 0, 1,
        [b"Alpha", b"THE red RED blue\n", b"a.txt", None, None, None])),
        (77, reader_reference.record(77, 0, 2,
        [b"RSS", b"Green blue", b"feed.xml", b"", b"Writer", b"raw date"]))]
    library = b"".join(value for _, value in records)
    entries = [(0, 0, len(records[0][1])), (77, len(records[0][1]), len(records[1][1]))]
    check(first["documents.bin"] == library, "independent pipeline record fields including RSS empty-present URL")
    check(first["offsets.bin"] == reader_reference.table(entries, len(library)), "independent pipeline offsets/CRC")
    terms = [b"blue", b"green", b"red"]
    check(first["search-lexicon.txt"] == reference.text_bytes(
        ["SIFTWING-LEXICON 1", "S 3"] + [f"T {i} {word.hex()}" for i, word in enumerate(terms)]),
        "independent pipeline term IDs")
    frequencies = {b"blue": 3, b"green": 1, b"red": 4}
    check(first["recommendation-dictionary.txt"] == reference.text_bytes(
        ["SIFTWING-RECOMMENDATION 1", "S 4 8 3"] +
        [f"T {i} {word.hex()} {frequencies[word]}" for i, word in enumerate(terms)]),
        "independent pipeline recommendation counts")
    character_ids = {}
    for i, word in enumerate(terms):
        for scalar in set(word):
            character_ids.setdefault(scalar, []).append(i)
    links = sum(map(len, character_ids.values()))
    check(first["recommendation-characters.txt"] == reference.text_bytes(
        ["SIFTWING-CHARACTERS 1", f"S {len(character_ids)} {links}"] +
        ["C " + str(c) + " " + " ".join(map(str, character_ids[c])) for c in sorted(character_ids)]),
        "independent pipeline characters")
    reference.check_weights(first["search-weights.txt"],
        [(0, b"Alpha", b"THE red RED blue\n", {b"blue": 1, b"red": 2}),
         (77, b"RSS", b"Green blue", {b"blue": 1, b"green": 1})], terms)
    sha = lambda value: hashlib.sha256(value).hexdigest()
    manifest = reference.text_bytes(["SIFTWING-SNAPSHOT 1", "I 42 roundtrip-pipeline-1",
        f"S 2 3 5 4 1 {sha(b'search:a.txt+feed.xml')} {sha(b'english:stop-the:simhash-distance-0')}",
        f"R 4 3 8 {len(character_ids)} {links} {sha(b'recommendation:a.txt+b.txt+feed.xml+z.txt')} {sha(b'english:stop-the:all-deliveries')}"] +
        [f"F {name} 1 {len(first[name])} {sha(first[name])}" for name in reference.NAMES])
    check(first["manifest.txt"] == manifest, "independent pipeline manifest selection/provenance/hashes")
    check(first["COMPLETE"] == reference.text_bytes([f"SIFTWING-COMPLETE 1 42 {sha(manifest)}"]),
          "independent pipeline completion binding")
    loaded(args.reader, parent / "pipeline-0", 42, first, records, 4, frequencies)
    rewritten = root / "pipeline-rewritten"
    rewritten.mkdir()
    run([args.republish, str(parent / "pipeline-0"), "42", str(rewritten), "42"])
    check(files(rewritten / "42") == first, "pipeline loaded objects republish identical complete snapshot")
    loaded(args.reader, rewritten, 42, first, records, 4, frequencies)
    return first


def main():
    parser = argparse.ArgumentParser()
    for name in ["writer", "reader", "republish", "pipeline", "parent"]:
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    generator = random.Random(1262026)
    words = [s.encode() for s in ["a", "A", "blue", "red", "重复", "😀😀", "𐀀", "é", "e\u0301", "99", "a+b"]]
    cases = [([], 0, {}), ([], 7, {"😀😀".encode(): 13}),
             ([(0, b"", b"", {}), (reference.MAX_ID, b"empty", b"\0\xff", {})], 5, {}),
             ([(0, b"title\0", b"body\xff", {b"red": 4}),
               (reference.MAX_ID, "原创".encode(), b"body", {b"red": 1, b"blue": 2})],
              9, {b"a": 3, "😀😀".encode(): 7})]
    for _ in range(28):
        ids = generator.sample([0, reference.MAX_ID, 7, 18, 19, 200, 1000], generator.randrange(7))
        documents = [(identity, ("原创-" + str(identity)).encode(), b"body\0\xff",
                      {word: generator.randrange(1, 99) for word in generator.sample(words, generator.randrange(7))})
                     for identity in ids]
        deliveries = generator.randrange(10)
        frequencies = {} if deliveries == 0 else {
            word: generator.randrange(1, 199) for word in generator.sample(words, generator.randrange(8))}
        cases.append((documents, deliveries, frequencies))
    root = pathlib.Path(tempfile.mkdtemp(prefix="snapshot-roundtrip-", dir=args.parent))
    aggregate = hashlib.sha256()
    try:
        for case, (documents, deliveries, frequencies) in enumerate(cases):
            version = [0, reference.MAX_ID, 42][case % 3]
            original = root / f"{case}-original"
            repeated = root / f"{case}-repeated"
            rewritten = root / f"{case}-rewritten"
            for parent in [original, repeated, rewritten]:
                parent.mkdir()
            run([args.writer, str(original), str(version)], wire=reference.wire(documents, deliveries, frequencies))
            reference.reference(original / str(version), version, documents, deliveries, frequencies)
            expected = files(original / str(version))
            permuted = [(d, title, body, dict(reversed(list(tf.items())))) for d, title, body, tf in reversed(documents)]
            run([args.writer, str(repeated), str(version)],
                wire=reference.wire(permuted, deliveries, dict(reversed(list(frequencies.items())))))
            check(files(repeated / str(version)) == expected, "independent process/permuted input fixed-metadata byte determinism")
            records = [(document[0], reference.encode_record(document)) for document in documents]
            loaded(args.reader, original, version, expected, records, deliveries, frequencies)
            loaded(args.reader, repeated, version, expected, records, deliveries, frequencies)
            run([args.republish, str(original), str(version), str(rewritten), str(version)])
            check(files(rewritten / str(version)) == expected, "Reader→Writer full eight-file identity")
            loaded(args.reader, rewritten, version, expected, records, deliveries, frequencies)
            retagged = root / f"{case}-new-version"
            retagged.mkdir()
            new_version = 1
            run([args.republish, str(original), str(version), str(retagged), str(new_version)])
            reference.reference(retagged / str(new_version), new_version, documents, deliveries, frequencies)
            changed = files(retagged / str(new_version))
            check(all(changed[name] == expected[name] for name in reference.NAMES), "version identity leaves six payloads unchanged")
            check(changed["manifest.txt"] != expected["manifest.txt"] and changed["COMPLETE"] != expected["COMPLETE"],
                  "new version changes manifest identity and completion binding")
            loaded(args.reader, retagged, new_version, changed, records, deliveries, frequencies)
            run([args.writer, str(original), str(version)], expected=1,
                wire=reference.wire(documents, deliveries, frequencies))
            check(files(original / str(version)) == expected, "duplicate publication preserves original snapshot")
            check(sorted(p.name for p in original.iterdir()) == [str(version)], "success and preexisting-version rejection leave no temporary directory")
            for name in NAMES:
                payload = expected[name]
                aggregate.update(struct.pack(">Q", len(payload)))
                aggregate.update(payload)
        pipeline_files = pipeline(args, root)
        for name in NAMES:
            aggregate.update(struct.pack(">Q", len(pipeline_files[name])))
            aggregate.update(pipeline_files[name])
        # 三个拒绝场景只改测试副本；成功对象往返与失败没有混用输出。
        original = root / "3-original"
        rejected = root / "rejected-output"
        rejected.mkdir()
        run([args.republish, str(original), "1", str(rejected), "1"], expected=1)
        check(not list(rejected.iterdir()), "wrong requested version produces no republished output")
        broken = root / "broken"
        broken.mkdir()
        shutil.copytree(original / "0", broken / "0")
        path = broken / "0" / "documents.bin"
        path.write_bytes(path.read_bytes()[:-1])
        run([args.republish, str(broken), "0", str(rejected), "0"], expected=1)
        check(not list(rejected.iterdir()), "truncated source produces no output")
        shutil.copytree(original / "0", broken / "42")
        path = broken / "42" / "manifest.txt"
        path.write_bytes(path.read_bytes().replace(b"SIFTWING-SNAPSHOT 1\n", b"SIFTWING-SNAPSHOT 2\n", 1))
        run([args.republish, str(broken), "42", str(rejected), "42"], expected=1)
        check(not list(rejected.iterdir()), "unsupported source produces no output")
    except BaseException:
        print("failed artifacts retained: " + str(root), flush=True)
        raise
    else:
        shutil.rmtree(root)  # 仅驱动本次创建的原创测试输出，失败产物保留。
    print(f"checks={CHECKS} reference_checks={reference.CHECKS} failures=0; cases={len(cases)}; canonical_sha256={aggregate.hexdigest()}")


if __name__ == "__main__":
    main()
