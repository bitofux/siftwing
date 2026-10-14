#!/usr/bin/env python3
"""真实生产server/TCP闭环；复用原创建库夹具与独立数学/Unicode参考。

期望由Python向量/二维距离/标量窗口生成；不以CLI或生产查询输出生成期望。
英文稀疏ID与中文并发均验证拥有Body；失败夹具保留路径，成功才回收。
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import cli_test as ref

CHECKS = 0

def check(value, label):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(label)


def wire(kind, identity, body):
    if not isinstance(body, bytes):
        body = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
    return struct.pack("!4sBBHHHIQ", b"SFTW", 1, 24, kind, 0, 0, len(body), identity) + body


def exact(peer, size):
    output = b""
    while len(output) < size:
        part = peer.recv(size - len(output))
        check(bool(part), "response truncated")
        output += part
    return output


def read(peer, kind, identity, status=0):
    raw = exact(peer, 24)
    magic, version, size, actual, flags, code, length, correlation = struct.unpack("!4sBBHHHIQ", raw)
    check((magic, version, size, actual, flags, code, correlation) ==
          (b"SFTW", 1, 24, kind, 0, status, identity), "independent header/identity/status")
    check(2 <= length <= 1048576, "bounded response body")
    body = exact(peer, length)
    check(not body.endswith(b"\n"), "network body has no CLI newline")
    return json.loads(body)


class Server:
    def __init__(self, parent, extra=()):
        self.process = subprocess.Popen([OPTIONS.server, "--snapshot", str(parent), "--version", "0", *map(str, extra)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if not select.select([self.process.stdout], [], [], 20)[0]:
            self.process.kill()
            raise AssertionError("startup timeout")
        line = self.process.stdout.readline()
        if not line.startswith(b"PORT "):
            _, err = self.process.communicate(timeout=20)
            raise AssertionError((line, err, self.process.returncode))
        self.port = int(line.split()[1])
    def peer(self):
        peer = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        peer.settimeout(15)
        return peer
    def __enter__(self):
        return self
    def __exit__(self, kind, value, trace):
        self.process.send_signal(signal.SIGTERM)
        output, error = self.process.communicate(timeout=20)
        if kind is None:
            check(self.process.returncode == 0 and not error, ("server stop", self.process.returncode, error))
            self.report = json.loads(output)
            check(self.report["active"] == 0 and self.report["workers_failed"] == 0, "all resources/tasks stopped")


def search(peer, identity, records, text, tf=None, mode="or", k=10, window=160, leading=40):
    peer.sendall(wire(3, identity, {"query": text, "mode": mode, "top_k": k,
                                 "summary_window": window, "summary_context": leading}))
    out = read(peer, 0x8003, identity)
    tf = ref.words(text) if tf is None else tf
    check(out["command"] == "SEARCH" and out["mode"] == mode and out["top_k"] == k, "business options")
    check(out["terms"] == [{"term": word, "tf": tf[word]} for word in sorted(tf)], "independent query TF")
    expected = ref.rank(records, tf, mode, k)
    check([int(hit["document_id"]) for hit in out["results"]] == [entry[0] for entry in expected], "independent ranking/DocId")
    for hit, (identity, score) in zip(out["results"], expected):
        record = next(item for item in records if item["id"] == identity)
        check(math.isclose(hit["score"], score, abs_tol=3e-15, rel_tol=3e-15), "independent cosine")
        check(hit["title"] == record["title"] and hit["url"] == record.get("url"), "same snapshot metadata")
        check(hit["summary"] == ref.display(record["body"], tf, window, leading), "independent scalar summary/highlight")
    check(out["schema_version"] == 1 and out["snapshot_version"] == "0", "lossless snapshot identity")
    return out


def suggest(peer, identity, frequencies, key, k=10, normalize=False, prepared=None):
    peer.sendall(wire(4, identity, {"key": key, "top_k": k, "normalize_key": normalize}))
    out = read(peer, 0x8004, identity)
    key = key if prepared is None else prepared
    expected = sorted((ref.distance(key, word), -count, word) for word, count in frequencies.items()
                      if set(word) & set(key))[:k]
    vocabulary = sorted(frequencies)
    check(out["key"] == key and out["normalized"] == normalize, "suggest key semantics")
    check(out["results"] == [{"term_id": str(vocabulary.index(word)), "term": word,
                              "distance": distance, "frequency": -count}
                             for distance, count, word in expected], "independent scalar DP/candidates/ties")
    return out


def integration(root):
    parent, records, frequencies = ref.golden(root)
    before = ref.inventory(root)
    with Server(parent) as server, server.peer() as peer:
        # 一次发送多帧并半关闭，固定头/Body及缓存继续由peer独立观测。
        peer.sendall(wire(1, 1, b"{}") + wire(3, 2, {"query": "unknown"}) + wire(4, 3, {"key": "z"}))
        check(read(peer, 0x8001, 1) == {}, "PING unchanged")
        check(read(peer, 0x8003, 2)["results"] == [] and read(peer, 0x8004, 3)["results"] == [], "empty business success")
        for i, (text, mode) in enumerate([(text, mode) for text in ["red", "RED red blue", "red absent", "green", "unknown"]
                                         for mode in ["and", "or"]], 10):
            search(peer, i, records, text, mode=mode)
        tie = search(peer, 30, records, "red", window=7, leading=2)
        check([hit["document_id"] for hit in tie["results"]] == ["0", "3"], "sparse identity tie")
        for i, key in enumerate(["cat", "CAT", "aa", "t", "z", "é", "中"], 40):
            suggest(peer, i, frequencies, key)
        picked = suggest(peer, 50, frequencies, " CAT ", normalize=True, prepared="cat")
        search(peer, 51, records, picked["results"][0]["term"])
        for window, leading in [(1, 0), (4, 0), (4096, 0)]:
            search(peer, 52, records, "red blue", window=window, leading=leading)
        # 分片跨头与Body；SHUT_WR后仍收到完整最后响应。
        packet = wire(3, 2**64-1, {"query": "red"})
        for offset in range(0, len(packet), 5): peer.sendall(packet[offset:offset+5])
        peer.shutdown(socket.SHUT_WR)
        check(len(read(peer, 0x8003, 2**64-1)["results"]) == 2, "fragmentation/half-close/max request id")
        check(peer.recv(1) == b"", "EOF after final response")
    check(ref.inventory(root) == before, "source and snapshot bytes remain unchanged")
    # 已装载拥有快照与资源；启动后路径改名，不影响worker纯内存调用。
    with Server(parent) as server:
        hidden = parent.with_name("hidden-loaded-snapshot")
        parent.rename(hidden)
        try:
            with server.peer() as peer: search(peer, 1, records, "red")
        finally: hidden.rename(parent)
    check(ref.inventory(root) == before, "loaded snapshot path restored exactly")


def chinese(root):
    dictionary = ref.write(root, "dict", "北京 100 ns\n中国 100 ns\n中文 100 n\n单 1 n\n")
    model = ref.write(root, "hmm", "0 -100 -100 -100\n-100 0 -100 -100\n0 -100 -100 -100\n"
                      "-100 0 -100 -100\n0 -100 -100 -100\n曙:0,光:0\n曙:0,光:0\n曙:0,光:0\n曙:-100,光:-100\n")
    a = ref.write(root, "a.txt", "中国北京 RED 曙光")
    b = ref.write(root, "b.txt", "中文北京 blue")
    resource = ["--search-tokenizer", "cppjieba", "--search-dictionary", dictionary, "--search-model", model]
    parent = ref.build(root, [("txt", a), ("txt", b)], [("txt", ref.write(root, "rec.txt", "cat cat bat"))], extra=resource)
    records = [{"id": 0, "title": "a", "body": a.read_text(), "tf": Counter({"中国": 1, "北京": 1, "red": 1, "曙光": 1})},
               {"id": 1, "title": "b", "body": b.read_text(), "tf": Counter({"中文": 1, "北京": 1, "blue": 1})}]
    return parent, records, ["--tokenizer", "cppjieba", "--dictionary", dictionary, "--model", model]


def concurrency(root):
    parent, records, resources = chinese(root)
    before = ref.inventory(root)
    with Server(parent, [*resources, "--subs", 3, "--workers", 4]) as server:
        # 资源已拥有，改名验证请求期间不重读；多个worker共享同一真实中文分词器。
        hidden = []
        for name in ["dict", "hmm"]:
            p = root/name; q = root/(name+"-loaded"); p.rename(q); hidden.append((p, q))
        try:
            def client(index):
                with server.peer() as peer:
                    for turn in range(12):
                        text, tf = ("中国北京 RED", Counter({"中国": 1, "北京": 1, "red": 1})) if index % 2 else ("北京", Counter({"北京": 1}))
                        search(peer, turn+1, records, text, tf=tf, mode="and" if index % 3 else "or")
                        suggest(peer, turn+1, Counter({"cat": 2, "bat": 1}), "cat")
            with ThreadPoolExecutor(max_workers=12) as pool: list(pool.map(client, range(24)))
        finally:
            for p, q in hidden: q.rename(p)
    check(server.report["workers_succeeded"] == 576, "each accepted business request executes once")
    check(ref.inventory(root) == before, "all Chinese resources/snapshot bytes unchanged")


def failures(root):
    parent, records, _ = ref.golden(root)
    before = ref.inventory(root)
    with Server(parent) as server, server.peer() as peer:
        bad = [b'{}', b'{"query":null}', b'{"query":5}', b'{"query":""}', b'{"query":"!!!"}',
               b'{"query":"red","top_k":0}', b'{"query":"red","top_k":101}',
               b'{"query":"red","top_k":1.0}', b'{"query":"red","top_k":true}',
               b'{"query":"red","mode":"AND"}', b'{"query":"red","path":"/tmp"}',
               b'{"query":"red","summary_window":1}', b'{"query":"red","summary_context":160}',
               b'{"query":"red","query":"green"}', b'{"query":"red","top_k":1,"top_k":2}',
               b'{"query":"red","mode":[]}', b'{"query":"red\\u0000"}']
        for i, body in enumerate(bad, 1):
            peer.sendall(wire(3, i, body)); check(read(peer, 0xffff, i, 1) == {"message":"invalid request"}, "fixed invalid error")
        for i, body in enumerate([{}, {"key":""}, {"key":"two words"}, {"key":"cat","normalize_key":1},
                                  {"key":"cat","query":"red"}], 40):
            peer.sendall(wire(4, i, body)); read(peer, 0xffff, i, 1)
        search(peer, 80, records, "red")
        peer.sendall(wire(2, 81, {})); read(peer, 0xffff, 81, 1)  # SERVER_INFO仍旧未接入行为。
    for flag, value, request in [("--max-output-bytes", 1, {"query":"red"}),
                                 ("--max-work", 1, {"query":"red blue"}),
                                 ("--max-query-bytes", 2, {"query":"red"})]:
        with Server(parent, [flag, value]) as server, server.peer() as peer:
            peer.sendall(wire(3, 1, request)); read(peer, 0xffff, 1, 6)
    with Server(parent, ["--max-top-k", 1]) as server, server.peer() as peer:
        peer.sendall(wire(3, 1, {"query":"red"})); read(peer, 0xffff, 1, 1)
        search(peer, 2, records, "red", k=1)
    invalid = ref.custom(root, "invalid-metadata", [{"id": 7, "title": b"\xff", "body": "red", "tf": ref.words("red")}])
    with Server(invalid) as server, server.peer() as peer:
        peer.sendall(wire(3, 1, {"query":"red"})); read(peer, 0xffff, 1, 4)
        peer.sendall(wire(4, 2, {"key":"cat"})); check(read(peer, 0x8004, 2)["results"] == [], "unrelated metadata not used")
    for arguments, expected in [([], 2), (["--snapshot", parent], 2),
        (["--snapshot", parent, "--version", "00"], 2),
        (["--snapshot", parent, "--version", "0", "--workers", "0"], 2),
        (["--snapshot", root/"missing", "--version", "0"], 1),
        (["--snapshot", parent, "--version", "0", "--stopwords", root/"missing"], 1),
        (["--snapshot", parent, "--version", "0", "--max-snapshot-bytes", "1"], 1)]:
        result = subprocess.run([OPTIONS.server, *map(str, arguments)], capture_output=True, timeout=20)
        check(result.returncode == expected and not result.stdout, "startup failure never publishes port")
    result = subprocess.run([OPTIONS.server, "--snapshot", str(parent), "--version", "0", "--run-ms", "1"], capture_output=True, timeout=20)
    check(result.returncode == 0 and b'"active":0' in result.stdout, "finite command stops resources")
    check(all(ref.inventory(root)[name] == sha for name, sha in before.items()), "original inputs kept")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ["server", "indexer", "writer", "native"]: parser.add_argument("--"+name, required=True)
    parser.add_argument("--suite", choices=["integration", "concurrency", "failures", "native"], required=True)
    OPTIONS = parser.parse_args()
    ref.OPTIONS = OPTIONS
    root = Path(tempfile.mkdtemp(prefix="siftwing-online-"))
    try:
        if OPTIONS.suite == "native":
            parent, _, _ = ref.golden(root)
            result = subprocess.run([OPTIONS.native, str(parent)], capture_output=True, timeout=30)
            print(result.stdout.decode(), end="")
            check(result.returncode == 0, (result.stdout, result.stderr))
        else: globals()[OPTIONS.suite](root)
    except BaseException:
        print("失败原创夹具保留：", root)
        raise
    else: shutil.rmtree(root)
    print(f"online {OPTIONS.suite} checks={CHECKS} fixture_checks={ref.CHECKS} failures=0")
