"""原创元数据独立生成规范字节；hashlib对照SHA-256，不从生产输出制造预期。"""
import argparse
import hashlib
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--probe", required=True)
args = parser.parse_args()
checks = 0


def run(mode, data, expected=None, rejected=False):
    global checks
    result = subprocess.run([args.probe, mode], input=data, capture_output=True, timeout=10)
    checks += 1
    if rejected:
        assert result.returncode == 1 and result.stderr.startswith(b"manifest."), result
    else:
        assert result.returncode == 0 and result.stdout == expected, (mode, data[:100], result)


# 覆盖单/双padding块分界、完整块、高位/NUL字节及长输入。
rng = random.Random(123)
hash_cases = [b"", b"abc", b"a" * 1000000, bytes(range(256))]
hash_cases += [rng.randbytes(n) if hasattr(rng, "randbytes") else bytes(rng.randrange(256) for _ in range(n))
               for n in list(range(130)) + [255, 256, 511, 512, 1023, 4096, 65536]]
for data in hash_cases:
    run("hash", data, hashlib.sha256(data).hexdigest().encode() + b"\n")

names = ["documents.bin", "offsets.bin", "search-lexicon.txt", "search-weights.txt",
         "recommendation-dictionary.txt", "recommendation-characters.txt"]
aggregate = hashlib.sha256()
for ordinal in range(65):
    n = ordinal % 7
    terms = 0 if n == 0 else ordinal % 5
    postings = terms * n
    tokens = postings * 3
    rt = ordinal % 9
    docs = ordinal + 1
    # 四份独立摘要不是搜索/推荐累计的替身。
    provenance = [hashlib.sha256(f"descriptor:{ordinal}:{i}".encode()).hexdigest() for i in range(4)]
    lines = ["SIFTWING-SNAPSHOT 1", f"I {2**64-1 if ordinal==64 else ordinal} build-{ordinal}",
             f"S {n} {terms} {tokens} {postings} 1 {provenance[0]} {provenance[1]}",
             f"R {docs} {rt} {rt*5} {rt} {rt*2} {provenance[2]} {provenance[3]}"]
    sizes = [88*n, 32+24*n, 1+ordinal, 2+ordinal, 3+ordinal, 4+ordinal]
    for i, name in enumerate(names):
        digest = hashlib.sha256(bytes([i]) * sizes[i]).hexdigest()
        lines.append(f"F {name} 1 {sizes[i]} {digest}")
    data = ("\n".join(lines) + "\n").encode()
    run("parse", data, data)
    run("complete", data, f"SIFTWING-COMPLETE 1 {2**64-1 if ordinal==64 else ordinal} {hashlib.sha256(data).hexdigest()}\n".encode())
    aggregate.update(data)
    bad_cases = [data[:-1], data+b"\n", data.replace(b"SNAPSHOT 1", b"SNAPSHOT 2", 1),
                 data.replace(b"F offsets.bin", b"F documents.bin", 1),
                 data.replace(b"F search-lexicon.txt", b"F ../evil", 1),
                 data.replace(b"\n", b"\r\n", 1),
                 data.replace(b"I ", b"I +", 1),
                 data.replace(b" 1 ", b" 01 ", 1),
                 data.replace(b"F documents.bin 1", b"F documents.bin 2", 1)]
    for bad in bad_cases:
        run("parse", bad, rejected=True)

print(f"checks={checks} failures=0; independent hashlib and 65 manifest batches; canonical_sha256={aggregate.hexdigest()}")
