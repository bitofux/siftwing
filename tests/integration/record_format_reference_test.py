"""用 struct 与 zlib 的独立格式参考验证编码和重算 CRC 后的恶意结构。"""
import argparse
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


def run(args, code=0):
    global checks
    result = subprocess.run([probe] + args, capture_output=True, text=True, timeout=5)
    checks += 1
    if result.returncode != code:
        raise RuntimeError((args[:5], result.returncode, result.stdout, result.stderr))
    return result.stdout.strip()


def crc(data):
    data[36:40] = struct.pack(">I", zlib.crc32(data[:36] + data[40:]) & 0xffffffff)
    return data


def reference(doc_id, ordinal, kind, flags, fields):
    payload = b"".join(struct.pack(">Q", len(value)) + value for value in fields)
    head = struct.pack(">4sHHQQQBBHI", b"SWDR", 1, 40, 40 + len(payload),
                       doc_id, ordinal, kind, flags, 0, 0)
    return bytes(crc(bytearray(head + payload)))


rng = random.Random(117)
for case in range(96):
    flags = case % 8
    fields = [bytes(rng.randrange(256) for _ in range(rng.randrange(32))) for _ in range(6)]
    for field in range(3, 6):
        if not flags & (1 << (field - 3)):
            fields[field] = b""
    if case < 8:
        fields = [b"", "中国😀".encode(), b"a\x00b"] + [b""] * 3
    doc_id = [0, 2**64 - 1, rng.getrandbits(64)][case % 3]
    ordinal = [2**64 - 1, 0, rng.getrandbits(64)][case % 3]
    kind = case % 3 + 1
    args = [str(doc_id), str(ordinal), str(kind), str(flags)] + [v.hex() for v in fields]
    expected = reference(doc_id, ordinal, kind, flags, fields)
    actual = bytes.fromhex(run(["encode"] + args))
    checks += 1
    if actual != expected:
        raise RuntimeError(("independent byte mismatch", case))
    info = run(["validate", "65536", "1048576", expected.hex()])
    checks += 1
    if info != f"{doc_id} {len(expected)}":
        raise RuntimeError(("metadata mismatch", case, info))
    if case == 0:
        # 临时输出在输入之外；成功要同时检查写、flush 和 close，然后逐字节比对真实文件。
        with tempfile.TemporaryDirectory(prefix="siftwing-record-") as directory:
            path = pathlib.Path(directory) / "temporary.bin"
            run(["write"] + args + [str(path)])
            checks += 1
            if path.read_bytes() != expected:
                raise RuntimeError("file bytes differ")
            # /dev/full 是当前 Linux 的实际写故障；可能由 write 或 flush 发现。
            run(["write"] + args + ["/dev/full"], 2)
            # 输出未打开：目录不能作为普通文件；临时目录由本测试负责清理。
            run(["write"] + args + [str(pathlib.Path(directory) / "missing" / "file")], 2)

valid = reference(7, 9, 2, 0, [b"t", b"content", b"a.xml", b"", b"", b""])
# 即使攻击者重算 CRC，未知头及内部长度仍不能通过，仅依赖校验码的实现会失败。
for offset, payload in [(0, b"BAD!"), (4, b"\x00\x02"), (6, b"\x00\x29"),
                        (32, b"\x00"), (32, b"\x04"), (33, b"\x08"), (34, b"\x00\x01"),
                        (40, struct.pack(">Q", 2**64 - 1)), (8, struct.pack(">Q", 1))]:
    broken = bytearray(valid)
    broken[offset:offset + len(payload)] = payload
    crc(broken)
    run(["validate", "65536", "1048576", broken.hex()], 2)
hidden = reference(7, 9, 2, 0, [b"t", b"c", b"a", b"hidden", b"", b""])
run(["validate", "65536", "1048576", hidden.hex()], 2)
extra = bytearray(valid + b"extra")
extra[8:16] = struct.pack(">Q", len(extra))
crc(extra)
run(["validate", "65536", "1048576", extra.hex()], 2)
run(["validate", "6", "1048576", valid.hex()], 2)
run(["validate", "65536", str(len(valid) - 1), valid.hex()], 2)
print(f"checks={checks} failures=0; independent struct/zlib bytes, corruption, real file and IO failures")
