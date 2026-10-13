#!/usr/bin/env python3
"""独立struct头oracle与原创黄金/Body预期；不调用生产encoder生成期望。"""
import argparse
import hashlib
import random
from pathlib import Path
import struct
import subprocess

HEADER = struct.Struct(">4sBBHHHIQ")
TYPES = {1, 2, 3, 4, 0x8001, 0x8002, 0x8003, 0x8004, 0xFFFF}
REQUESTS = {1, 2, 3, 4}
HARD_LIMIT = 1048576


def wire(kind=3, status=0, identity=0x0102030405060708, body=b"{}", **fields):
    values = [b"SFTW", 1, 24, kind, 0, status, len(body), identity]
    for key, index in {"magic": 0, "version": 1, "header": 2, "flags": 4, "length": 6}.items():
        if key in fields:
            values[index] = fields[key]
    return HEADER.pack(*values) + body


def hx(raw):
    return raw.hex() if raw else "-"


def decode_reference(raw, role, limit, budget, body_result="ok"):
    # 语义表来自公开协议；用struct解包而不是复制C++位移实现，错误前缀不发布Frame。
    if len(raw) < HEADER.size:
        return "N " + hx(raw)
    magic, version, header, kind, flags, status, length, identity = HEADER.unpack(raw[:24])
    conditions = [(magic != b"SFTW", "magic"), (version != 1, "version"),
                  (header != 24, "header_size"), (kind not in TYPES, "message_type"),
                  (flags != 0, "flags"),
                  ((status not in range(1, 7)) if kind == 0xFFFF else status != 0, "status"),
                  (not 2 <= length <= limit, "body_length"), (identity == 0, "request_id"),
                  ((kind in REQUESTS) != (role == "s"), "direction"),
                  (24 + length > budget, "buffer_limit")]
    reason = next((label for bad, label in conditions if bad), None)
    if reason:
        return "E " + reason + " " + hx(raw)
    end = 24 + length
    if len(raw) < end:
        return "N " + hx(raw)
    return f"F {kind} {status} {identity} {hx(raw[24:end])} {hx(raw[end:])} {body_result}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe")
    parser.add_argument("--write-fuzz-seeds")
    args = parser.parse_args()
    if args.write_fuzz_seeds:
        destination = Path(args.write_fuzz_seeds)
        # 新建外部种子目录，不覆盖已有corpus；不读私有输入或课程资料。
        destination.mkdir()
        bodies = [b"{}", b"{]", b"[]", b"{}\x00hidden", b'\xef\xbb\xbf{}',
                  b'{"x":"\xc0\xaf"}', b'{"x":"\\u0000"}', b'{"x":"\\uD800"}',
                  b'{"x":1e999}', ' {"文":"字"} '.encode()]
        for depth in [63, 64, 65, 1000]:
            bodies.append(b'{"x":' + b'[' * (depth - 1) + b'0' + b']' * (depth - 1) + b'}')
        seeds = set(bodies)
        for kind in sorted(TYPES):
            status = 1 if kind == 0xFFFF else 0
            for body in bodies:
                seeds.add(wire(kind, status, body=body))
        ping = wire(1)
        seeds.update(ping[:i] for i in range(1, len(ping) + 1))
        seeds.update([ping + ping + ping[:23], wire(version=2), wire(length=0xFFFFFFFF)])
        for seed in seeds:
            (destination / hashlib.sha256(seed).hexdigest()).write_bytes(seed)
        print(f"fuzz_seeds={len(seeds)} destination={destination}")
        return
    if not args.probe:
        parser.error("--probe or --write-fuzz-seeds is required")
    commands, expected = [], []

    def decode(raw, role="s", limit=HARD_LIMIT, budget=None, body_result="ok"):
        capacity = max(len(raw), 24 + HARD_LIMIT) if budget is None else budget
        commands.append(f"D {role} {limit} {capacity} {hx(raw)}")
        expected.append(decode_reference(raw, role, limit, capacity, body_result))

    def encode(kind, status, identity, body, role, limit=HARD_LIMIT, error=None):
        commands.append(f"E {role} {limit} {kind} {status} {identity} {hx(body)}")
        expected.append("P " + error if error else "W " + wire(kind, status, identity, body).hex())

    # 手工公开PING/PONG黄金进一步核对struct oracle，防止同一辅助构造整体写错布局。
    ping = bytes.fromhex("5346545701180001000000000000000201020304050607087b7d")
    pong = bytes.fromhex("5346545701188001000000000000000201020304050607087b7d")
    if wire(1) != ping or wire(0x8001) != pong:
        raise RuntimeError("independent manual and struct golden disagree")
    for kind in sorted(TYPES):
        for status in (range(1, 7) if kind == 0xFFFF else [0]):
            for identity in [1, 0x0102030405060708, (1 << 64) - 1]:
                for body in [b"{}"] if kind in {1, 0x8001} else [b"{}", ' \n{"文":"字","nul":"\\u0000"}\t'.encode()]:
                    role = "s" if kind in REQUESTS else "c"
                    encode(kind, status, identity, body, role)
                    raw = wire(kind, status, identity, body)
                    for local_role in ["s", "c"]:
                        decode(raw, local_role)
                    for stop in range(len(raw) + 1):
                        decode(raw[:stop], role)
                    # 首帧后接第二帧和半头，验证不消费后续数据。
                    decode(raw + raw + raw[:11], role)

    # 一次验证整个16位编号空间，以及两类status的整个16位空间，预期明确拒绝而非只看不崩溃。
    for kind in range(65536):
        decode(wire(kind, 1 if kind == 0xFFFF else 0), "s" if kind in REQUESTS else "c")
    for status in range(65536):
        decode(wire(3, status), "s")
        decode(wire(0xFFFF, status), "c")

    base = wire()
    for at in range(24):
        for value in range(256):
            raw = bytearray(base)
            raw[at] = value
            for role in ["s", "c"]:
                decode(bytes(raw), role)
    for length in [0, 1, 2, 3, 255, 256, 65535, 65536, HARD_LIMIT - 1, HARD_LIMIT, HARD_LIMIT + 1, 0xFFFFFFFF]:
        raw = wire(length=length)
        for role in ["s", "c"]:
            for limit in [2, 3, HARD_LIMIT]:
                for budget in [len(raw), 24 + HARD_LIMIT]:
                    decode(raw, role, limit, budget)
    for fields in [{"magic": b"XXXX", "version": 2, "length": 0xFFFFFFFF},
                   {"version": 2, "header": 25, "flags": 1},
                   {"flags": 1, "length": 0xFFFFFFFF}]:
        decode(wire(**fields))

    # Body分类是手写原创用例预期，不能用Python宽松JSON库吞NUL/NaN代替冻结准入政策。
    bodies = [(b"{]", "json"), (b"{}\x00hidden", "json"), (b"\x00{}", "json"),
              (b"\xef\xbb\xbf{}", "json"), (b"{}{}", "json"),
              (b'{"x":"\xc0\xaf"}', "utf8"), (b'{"x":"\xed\xa0\x80"}', "utf8"),
              (b'{"x":"\xf4\x90\x80\x80"}', "utf8"), (b'[]', "json_object"),
              (b'true', "json_object"), (b'{"x":1e999}', "json"),
              (b'{"x":"\\uD800"}', "json"), (b'{"x":"\\uDC00"}', "json"),
              (b'{"x":"\\uD83D\\uDE00"}', "ok"), (b'{"x":"\\u0000"}', "ok"),
              (b'{"x":1,"x":2}', "ok")]
    for depth in [63, 64, 65, 1000]:
        body = b'{"x":' + b'[' * (depth - 1) + b'0' + b']' * (depth - 1) + b'}'
        bodies.append((body, "ok" if depth <= 64 else "json_depth"))
    for body, result in bodies:
        decode(wire(body=body), body_result=result)
        encode(3, 0, 1, body, "s", error=None if result == "ok" else result)
    for body in [b" {}", b"{ }", b'{"x":1}']:
        decode(wire(1, body=body), body_result="heartbeat")
        encode(1, 0, 1, body, "s", error="heartbeat")
    for length in [HARD_LIMIT - 1, HARD_LIMIT, HARD_LIMIT + 1]:
        body = b"{}" + b" " * (length - 2)
        decode(wire(body=body))
        encode(3, 0, (1 << 64) - 1, body, "s", error="body_length" if length > HARD_LIMIT else None)
    rng = random.Random(205)
    # 随机头保持Body {}以使header oracle能作精确判定；任意Body由性质fuzzer另负责。
    for _ in range(10000):
        raw = bytes(rng.randrange(256) for _ in range(24)) + b"{}"
        decode(raw, rng.choice(["s", "c"]))
    payload = "\n".join(commands) + "\n"
    result = subprocess.run([args.probe], input=payload, text=True, encoding="ascii", capture_output=True, timeout=100)
    if result.returncode:
        raise RuntimeError(f"probe exited {result.returncode}: {result.stderr}")
    observed = result.stdout.splitlines()
    if len(observed) != len(expected):
        raise RuntimeError(f"probe output count {len(observed)} != {len(expected)}")
    for index, (actual, wanted) in enumerate(zip(observed, expected)):
        if actual != wanted:
            raise RuntimeError(f"case {index}: {commands[index][:220]} expected {wanted[:220]} observed {actual[:220]}")
    digest = hashlib.sha256(payload.encode("ascii")).hexdigest()
    print(f"reference_cases={len(expected)} failures=0 seed=205 command_sha256={digest}")


if __name__ == "__main__":
    main()
