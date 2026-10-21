#!/usr/bin/env python3
"""网络CLI合同：原创快照的独立数学参考，以及手工线字节的恶意/分片对端。

不将生产CLI输出作为期望；失败夹具保留，成功才回收。总期限只设宽松调度上界，
进展频率与总期限分离；IPv6不可用时明确报告，不以跳过冒充覆盖。
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import cli_test as ref
import online_search_test as online

CHECKS = 0
SKIPS = []


def check(value, label):
    global CHECKS
    CHECKS += 1
    if not value:
        raise AssertionError(label)


def invoke(port, command="PING", extra=(), expected=0, stdout=subprocess.PIPE, host="127.0.0.1"):
    result = subprocess.run([OPTIONS.cli, "--network", command, "--host", host, "--port", str(port),
                             *map(str, extra)], stdout=stdout, stderr=subprocess.PIPE, timeout=10)
    check(result.returncode == expected, (command, extra, result.returncode, result.stdout, result.stderr))
    if expected == 3:
        check(b"cli.output" in result.stderr, "stdout failure has diagnostic")
    if stdout == subprocess.PIPE:
        if expected:
            check(result.stdout == b"" and bool(result.stderr), "failure has no partial stdout")
        else:
            check(result.stderr == b"" and result.stdout.count(b"\n") == 1 and result.stdout.endswith(b"\n"),
                  "one complete JSON line without diagnostic")
    return result


def frame(kind=0x8001, identity=1, body=b"{}", status=0, **change):
    fields = dict(magic=b"SFTW", version=1, size=24, kind=kind, flags=0, status=status,
                  length=len(body), identity=identity)
    fields.update(change)
    return struct.pack("!4sBBHHHIQ", *fields.values()) + body


def exact(peer, count):
    data = b""
    while len(data) < count:
        part = peer.recv(count-len(data))
        if not part:
            raise AssertionError("request truncated")
        data += part
    return data


def fake(action, command="PING", extra=(), expected=0, host="127.0.0.1", stdout=subprocess.PIPE,
         request_body=None):
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_STREAM) as listener:
        listener.bind((host, 0)); listener.listen(1); listener.settimeout(5)
        errors = []
        def serve():
            try:
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    header = struct.unpack("!4sBBHHHIQ", exact(peer, 24))
                    body = exact(peer, header[6])
                    check(header[:6] == (b"SFTW", 1, 24,
                          {"PING":1, "SERVER_INFO":2, "SEARCH":3, "SUGGEST":4}[command], 0, 0),
                          "independent request wire header")
                    identity = int(extra[extra.index("--request-id")+1]) if "--request-id" in extra else 1
                    check(header[7] == identity, "full uint64 request identity")
                    check(json.loads(body) == (request_body or {}), "exact request JSON semantics")
                    action(peer)
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=serve)
        thread.start()
        try:
            result = invoke(listener.getsockname()[1], command, extra, expected, stdout, host)
        finally:
            thread.join(6)
        check(not thread.is_alive() and not errors, ("fake server", errors))
        return result


def protocol(root):
    def fragmented(peer):
        for byte in frame():
            peer.sendall(bytes([byte])); time.sleep(.001)
    check(fake(fragmented).stdout == b"{}\n", "split header and body")
    for command, kind, body, options in [
        ("SERVER_INFO", 0x8002, {}, ()),
        ("SEARCH", 0x8003, {"query":"中国 RED", "mode":"and", "top_k":2,
                             "summary_window":4,"summary_context":0},
         ("--query","中国 RED","--mode","and","--top-k","2","--summary-window","4","--summary-context","0")),
        ("SUGGEST", 0x8004, {"key":"CAT", "top_k":1,"normalize_key":True},
         ("--key","CAT","--top-k","1","--normalize-key"))]:
        # Body schema在服务端解释；客户端只校验协议JSON对象并逐字节输出。
        raw = b'{ "ok":true,"text":"\\u4e2d" }'
        result = fake(lambda p: p.sendall(frame(kind, 2**64-1, raw)), command,
                      (*options,"--request-id",str(2**64-1)), request_body=body)
        check(result.stdout == raw+b"\n", "preserve complete raw response bytes")
    for status in range(1, 7):
        out = fake(lambda p, s=status: p.sendall(frame(0xffff, body=b'{"message":"remote"}', status=s)), expected=5)
        check(f"status={status}".encode() in out.stderr, "remote status retained")
    bad = [frame(kind=0x8002), frame(identity=2), frame(identity=0), frame(kind=1), frame(kind=0x7777),
           frame(magic=b"NOPE"), frame(version=2), frame(size=23), frame(flags=1), frame(status=1),
           frame(kind=0xffff,status=7), frame(body=b"{\xff}"), frame(body=b"[]"), frame(body=b"{]"),
           frame(body=b' { }'), frame(length=1), frame(length=1048577), frame()[:13], frame()[:-1],
           frame()+b"junk"]
    for raw in bad:
        fake(lambda p, b=raw: p.sendall(b), expected=1)
    fake(lambda p: p.sendall(frame(0x8002, body=b'{"ok":true}')), "SERVER_INFO",
         ("--max-response-bytes","2"), expected=1)
    fake(lambda p: p.sendall(frame()), extra=("--max-response-bytes","2"))
    def reset(peer):
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    fake(reset, expected=1)
    def slow(peer):
        try:
            for byte in frame():
                peer.sendall(bytes([byte])); time.sleep(.04)
        except (BrokenPipeError, ConnectionResetError):
            pass
    started = time.monotonic()
    out = fake(slow, extra=("--timeout-ms","180"), expected=1)
    check(b"client.timeout" in out.stderr and time.monotonic()-started < 2.5,
          "progress cannot refresh fixed total deadline")
    with open("/dev/full", "wb") as output:
        fake(lambda p: p.sendall(frame()), expected=3, stdout=output)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
        reserved.bind(("127.0.0.1",0))
        invoke(reserved.getsockname()[1], expected=1)
    try:
        probe = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        probe.bind(("::1",0)); probe.close()
    except OSError as error:
        SKIPS.append("IPv6 loopback unavailable: "+str(error))
    else:
        fake(lambda p: p.sendall(frame()), host="::1")


def integration(root):
    parent, records, frequencies = ref.golden(root)
    before = ref.inventory(root)
    with online.Server(parent, ("--max-connections",256,"--completion-capacity",256,"--workers",2)) as server:
        check(invoke(server.port).stdout == b"{}\n", "real PING")
        info = json.loads(invoke(server.port,"SERVER_INFO").stdout)
        check(info == {"schema_version":1,"command":"SERVER_INFO","protocol_version":1,
                       "snapshot_version":"0","build_version":"original-cli-golden-1",
                       "commands":["PING","SERVER_INFO","SEARCH","SUGGEST"],
                       "limits":{"max_body_bytes":1048576,"max_query_bytes":65536,"max_top_k":100},
                       "cache_enabled":False}, "fixed public metadata without paths")
        for text, mode, k in [("RED blue","or",10),("red blue","and",1),("missing","or",3)]:
            out = json.loads(invoke(server.port,"SEARCH",("--query",text,"--mode",mode,"--top-k",k,
                         "--summary-window",12,"--summary-context",2)).stdout)
            tf = ref.words(text); expected = ref.rank(records,tf,mode,k)
            check(out["terms"] == [{"term":w,"tf":tf[w]} for w in sorted(tf)], "independent query terms")
            check([int(h["document_id"]) for h in out["results"]] == [v[0] for v in expected], "independent rank")
            for hit,(identity,score) in zip(out["results"],expected):
                record = next(r for r in records if r["id"] == identity)
                check(math.isclose(hit["score"],score,abs_tol=3e-15,rel_tol=3e-15), "independent cosine")
                check(hit["summary"] == ref.display(record["body"],tf,12,2), "independent Unicode display")
        for key,normalized,extra in [("cat","cat",()),("CAT","cat",("--normalize-key",))]:
            out=json.loads(invoke(server.port,"SUGGEST",("--key",key,"--top-k",3,*extra)).stdout)
            expected=sorted((ref.distance(normalized,w),-n,w) for w,n in frequencies.items()
                            if set(w)&set(normalized))[:3]
            check(out["key"] == normalized and out["results"] == [
                {"term_id":str(sorted(frequencies).index(w)),"term":w,"distance":d,"frequency":-n}
                for d,n,w in expected], "independent recommendation ordering")
        invoke(server.port,"SEARCH",("--query","!!!"),expected=5)
        invoke(server.port,"SUGGEST",("--key","two words"),expected=5)
        invoke(server.port,"SERVER_INFO",("--max-response-bytes",2),expected=1)
        # 正式Sub直接处理SERVER_INFO：空白对象合法，未知字段拒绝，单在途后继续服务。
        with server.peer() as peer:
            peer.sendall(online.wire(2,91,b' { } ')+online.wire(2,92,{"path":"secret"})+online.wire(1,93,{}))
            check(online.read(peer,0x8002,91) == info,"empty object semantic")
            online.read(peer,0xffff,92,1); online.read(peer,0x8001,93)
    check(ref.inventory(root)==before,"input and snapshot remain unchanged")


def arguments(root):
    cases=[[],["--network"],["--network","WHAT"],["--network","PING"],
           ["--network","SEARCH","--port","1"],["--network","SUGGEST","--port","1"]]
    for flags in [("--port","0"),("--port","65536"),("--port","01"),("--port","-1"),
                  ("--port","18446744073709551616"),("--host","localhost"),("--timeout-ms","0"),
                  ("--timeout-ms","86400001"),("--request-id","0"),("--max-response-bytes","1"),
                  ("--query","x"),("--snapshot","x"),("--port","1"),("--normalize-key",)]:
        cases.append(["--network","PING","--port","1",*flags])
    for flags in [("--mode","AND"),("--top-k","0"),("--summary-window","40"),
                  ("--summary-context","160"),("--key","red"),("--top-k",)]:
        cases.append(["--network","SEARCH","--port","1","--query","red",*flags])
    for args in cases:
        result=subprocess.run([OPTIONS.cli,*args],capture_output=True,timeout=5)
        check(result.returncode==2 and result.stdout==b"" and result.stderr,
              ("invalid arguments",args,result.returncode,result.stderr))
    help_result=subprocess.run([OPTIONS.cli,"--help"],capture_output=True,timeout=5)
    check(help_result.returncode==0 and b"--network" in help_result.stdout,"help exposes both modes")


if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    for name in ("cli","server","indexer","writer","suite"): parser.add_argument("--"+name,required=True)
    OPTIONS=parser.parse_args(); ref.OPTIONS=OPTIONS; online.OPTIONS=OPTIONS
    root=Path(tempfile.mkdtemp(prefix="siftwing-network-client-"))
    try:
        globals()[OPTIONS.suite](root)
    except BaseException:
        print("FAILED_FIXTURE",root,flush=True)
        raise
    else:
        shutil.rmtree(root)
        print(json.dumps({"suite":OPTIONS.suite,"checks":CHECKS,"reference_checks":ref.CHECKS,"skips":SKIPS}))
