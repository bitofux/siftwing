"""独立 struct/zlib 生成记录库/偏移表，验证完整字段与攻击性长度；不用生产编码取预期。"""
import argparse
import json
import pathlib
import random
import struct
import subprocess
import tempfile
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--probe", required=True)
probe = parser.parse_args().probe
checks = 0


def check(condition, label):
    global checks
    checks += 1
    if not condition:
        raise RuntimeError(label)


def run(args, code=0):
    result = subprocess.run([probe] + list(map(str, args)), capture_output=True, text=True, timeout=5)
    check(result.returncode == code, (args[:3], result.returncode, result.stdout, result.stderr))
    return result.stdout.strip()


def checksum(data, offset):
    data[offset:offset + 4] = struct.pack(">I", zlib.crc32(data[:offset] + data[offset + 4:]) & 0xffffffff)
    return bytes(data)


def record(doc_id, ordinal, kind, flags, fields):
    payload = b"".join(struct.pack(">Q", len(f)) + f for f in fields)
    header = struct.pack(">4sHHQQQBBHI", b"SWDR", 1, 40, len(payload) + 40,
                         doc_id, ordinal, kind, flags, 0, 0)
    return checksum(bytearray(header + payload), 36)


def table(entries, length):
    header = struct.pack(">4sHHQQII", b"SWDI", 1, 32, len(entries), length, 0, 0)
    return checksum(bytearray(header + b"".join(struct.pack(">QQQ", *e) for e in entries)), 24)


rng = random.Random(118)
with tempfile.TemporaryDirectory(prefix="siftwing-offset-reference-") as directory:
    path = pathlib.Path(directory) / "library.bin"
    for case in range(32):
        count = case % 10
        ids = [0, 2**64 - 1][:count]
        while len(ids) < count:
            value = rng.getrandbits(64)
            if value not in ids:
                ids.append(value)
        rng.shuffle(ids)
        library, entries, expected_records = b"", [], {}
        for index, doc_id in enumerate(ids):
            flags, kind = index % 8, index % 3 + 1
            fields = [bytes(rng.randrange(256) for _ in range(rng.randrange(32))) for _ in range(6)]
            if index == 0:
                fields[:3] = ["标题😀".encode(), b"a\x00b\xff", b"relative.xml"]
            for f in range(3, 6):
                if not flags & (1 << (f - 3)):
                    fields[f] = b""
                elif case % 2 == 0:
                    fields[f] = b""  # 已提供空值与缺失必须分别返回。
            ordinal = 2**64 - 1 if index == 0 else rng.getrandbits(64)
            data = record(doc_id, ordinal, kind, flags, fields)
            entries.append((doc_id, len(library), len(data)))
            library += data
            expected_records[doc_id] = {"id": doc_id, "ordinal": ordinal, "kind": kind,
                                       "fields": [f.hex() if i < 3 or flags & (1 << (i-3)) else None
                                                  for i, f in enumerate(fields)]}
        entries.sort()
        expected = table(entries, len(library))
        path.write_bytes(library)
        actual = bytes.fromhex(run(["build", path]))
        check(actual == expected, ("independent table bytes", case))
        check(bytes.fromhex(run(["validate", expected.hex()])) == expected, "canonical codec roundtrip")
        check(run(["inspect", path, expected.hex()]) == str(count), "whole-library content correspondence")
        for doc_id in reversed(ids):
            value = json.loads(run(["read", path, expected.hex(), doc_id]))
            check(value == expected_records[doc_id], ("full owned document fields", case, doc_id))
        run(["read", path, expected.hex(), 7], 2)
        check(path.read_bytes() == library, "read/build leaves library unchanged")

    library = record(0, 0, 1, 0, [b""] * 6) + record(1, 1, 2, 7, [b""] * 6)
    entries = [(0, 0, 88), (1, 88, 88)]
    valid = table(entries, 176)
    path.write_bytes(library)
    run(["build", path, 1, 88, 2, 176, 80])  # 框架也计入预算，恰好边界合法。
    for limits in [(0,88,2,176,80), (1,0,2,176,80), (1,88,0,176,80),
                   (1,88,2,0,80), (1,88,2,176,0), (1,87,2,176,80),
                   (1,88,1,176,80), (1,88,2,175,80), (1,88,2,176,79)]:
        run(["build", path] + list(limits), 2)
    for offset, value in [(0,b"BAD!"),(4,b"\x00\x02"),(6,b"\x00\x21"),(28,b"\x01"),
                          (8,struct.pack(">Q",2**64-1)), (16,struct.pack(">Q",175))]:
        broken = bytearray(valid)
        broken[offset:offset + len(value)] = value
        broken = checksum(broken, 24)
        run(["validate", broken.hex()], 2)
    for invalid_entries in [[(0,0,88),(0,88,88)], [(1,0,88),(0,88,88)],
                            [(0,0,88),(1,87,88)], [(0,0,88),(1,89,88)],
                            [(0,0,87),(1,88,88)], [(0,0,2**64-1),(1,88,88)],
                            [(0,2**64-1,88),(1,88,88)]]:
        run(["validate", table(invalid_entries,176).hex()], 2)
    # CRC 正确、覆盖正确的等长错位，只有实际记录 ID 对应检查能拒绝。
    wrong = table([(0,88,88),(1,0,88)],176)
    run(["validate", wrong.hex()])
    check(run(["inspect", path, wrong.hex()], 2) == "store.identity", "wrong equal-size association rejected")
    for cut in [0,1,39,40,87,88,89,175]:
        path.write_bytes(library[:cut])
        run(["inspect", path, valid.hex()], 2)
        if cut in [0,88]:
            run(["build", path])  # 独立扫描可接纳完整前缀；原表仍必须拒绝。
        else:
            run(["build", path], 2)
    path.write_bytes(library + b"tail")
    run(["build", path], 2)
    path.write_bytes(library + library[:88])
    run(["build", path], 2)
    for offset in [0,4,6,16,24,32,33,34,36,40,87,175]:
        broken = bytearray(library)
        broken[offset] ^= 1
        path.write_bytes(broken)
        run(["build", path], 2)
        run(["inspect", path, valid.hex()], 2)
    path.write_bytes(library)
    # count*24 溢出和 header+payload 溢出都在条目分配之前拒绝。
    for count in [2**64-1, (2**64-1)//24]:
        broken = bytearray(valid)
        broken[8:16] = struct.pack(">Q", count)
        run(["validate", checksum(broken,24).hex(), 2**64-1,2**64-1,2**64-1,2**64-1,2**64-1], 2)
    run(["validate", (valid+b"tail").hex()], 2)
    run(["build", pathlib.Path(directory)], 2)
    run(["build", pathlib.Path(directory)/"missing"], 2)
    link = pathlib.Path(directory)/"link"
    link.symlink_to(path)
    run(["build", link], 2)
print(f"checks={checks} failures=0; independent struct/zlib, random reads, budgets, corruption and correspondence")
