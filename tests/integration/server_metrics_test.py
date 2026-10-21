#!/usr/bin/env python3
"""原创快照+独立网络编码，核运行期/关闭指标的精确口径与导出过滤。"""
import argparse
import json
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import cli_test as ref
import online_search_test as online


def check(value, label):
    if not value:
        raise AssertionError(label)


def run(root, parent, mode):
    capture = root / (mode + '.jsonl')
    level = mode if mode in ('off', 'warn') else 'info'
    with open('/dev/full' if mode == 'full' else capture, 'wb') as output:
        proc = subprocess.Popen([OPTIONS.server, '--snapshot', str(parent), '--version', '0',
                                 '--log-level', level, '--cache-capacity', '16'],
                                stdout=subprocess.PIPE, stderr=output)
        tx = rx = 0
        try:
            check(select.select([proc.stdout], [], [], 20)[0], '启动期限')
            line = proc.stdout.readline()
            check(line.startswith(b'PORT '), ('PORT', line))
            port = int(line.split()[1])
            def connect():
                peer = socket.create_connection(('127.0.0.1', port), timeout=5)
                peer.settimeout(5)
                return peer
            def exchange(peer, kind, identity, body, response_kind=None, status=0):
                nonlocal tx, rx
                packet = online.wire(kind, identity, body)
                tx += len(packet)
                peer.sendall(packet)
                header = online.exact(peer, 24)
                magic, version, size, actual, flags, code, length, correlation = struct.unpack('!4sBBHHHIQ', header)
                check((magic, version, size, actual, flags, code, correlation) ==
                      (b'SFTW', 1, 24, response_kind or (kind | 0x8000), 0, status, identity), '独立响应头')
                data = online.exact(peer, length)
                rx += 24 + length
                return json.loads(data)
            with connect() as peer:
                exchange(peer, 1, 1, {})
                info = exchange(peer, 2, 2, {})
                check(info['snapshot_version'] == '0', '原INFO保持')
                first = exchange(peer, 3, 3, {'query': 'red'})
                check(first == exchange(peer, 3, 4, {'query': 'red'}), '缓存响应不变')
                exchange(peer, 4, 5, {'key': 'cat'})
                exchange(peer, 3, 6, {'query': 'PRIVATE-QUERY', 'unknown': True}, 0xffff, 1)
                exchange(peer, 1, 7, b'{{', 0xffff, 1)
                exchange(peer, 1, 8, {})
                # 轮询已flush的指标事件，确认关闭之前确实产生完整周期样本。
                if mode == 'info':
                    end = time.monotonic() + 5
                    while time.monotonic() < end:
                        if b'"metric_family":"shutdown"' in capture.read_bytes():
                            break
                        time.sleep(.02)
                    else:
                        raise AssertionError('缺运行期样本')
                peer.shutdown(socket.SHUT_WR)
                check(peer.recv(1) == b'', '完整输入排空')
            with connect() as peer:
                bad = b'FAIL' + online.wire(1, 9, {})[4:]
                tx += len(bad)
                peer.sendall(bad)
                check(peer.recv(1) == b'', '坏头局部关闭')
            with connect() as peer:
                partial = online.wire(1, 10, {})[:10]
                tx += len(partial)
                peer.sendall(partial)
                peer.shutdown(socket.SHUT_WR)
                check(peer.recv(1) == b'', '截帧局部关闭')
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            try:
                out, _ = proc.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
                raise
            (root / (mode + '.stdout')).write_bytes(out)
        check(proc.returncode == 0 and json.loads(out)['responses'] == 8, '坏输入/坏输出不破坏业务')
    if mode == 'full':
        return
    rows = [json.loads(raw) for raw in capture.read_bytes().splitlines()]
    metrics = [row for row in rows if row['event'] == 'metrics_snapshot']
    if mode in ('off', 'warn'):
        check(not metrics, '级别过滤指标')
        return
    groups = {}
    for row in metrics:
        check(row['metrics_schema_version'] == 1 and row['request_id'] is None
              and row['connection_id'] is None and row['index_version'] == '0', '固定版本/无请求标签')
        group = groups.setdefault(int(row['sample_id']), {})
        check(row['metric_family'] not in group, '同一样本族唯一')
        group[row['metric_family']] = row['values']
    check(len(groups) >= 2 and list(groups) == sorted(groups), '周期/最终递增样本')
    for group in groups.values():
        check(set(group) == {'connections', 'network', 'requests', 'queues', 'workers',
                             'search', 'suggest', 'cache', 'index', 'shutdown'}, '10族齐全')
        for family, values in group.items():
            for key, value in values.items():
                if isinstance(value, bool):
                    continue
                check(all(isinstance(n, str) and n.isdigit() for n in
                          (value if isinstance(value, list) else [value])), '计数完整十进制字符串')
    check(not groups[min(groups)]['shutdown']['started'], '运行期样本先于关闭')
    last = groups[max(groups)]
    def numbers(family):
        return {key: ([int(n) for n in val] if isinstance(val, list) else
                      val if isinstance(val, bool) else int(val)) for key, val in last[family].items()}
    c, n, q, w = map(numbers, ('connections', 'network', 'queues', 'workers'))
    check(c == dict(accepted=3, handed_off=0, rejected=0, admitted=3, closed=3, active=0, discarded=0), '精确连接')
    check(n['received_bytes'] == tx and n['sent_bytes'] == rx and n['protocol_errors'] == 3,
          ('精确网络/协议错误', n, tx, rx))
    check(n['input_bytes'] == n['output_bytes'] == 0 and n['input_high_bytes'] >= 26
          and n['output_high_bytes'] >= 26, '当前缓冲与历史峰值分离')
    check(numbers('requests') == dict(by_type=[3, 1, 3, 1], responses=[6, 2, 0, 0, 0, 0, 0], drained=8), '受信请求/状态码')
    check(q['tasks'] == q['handoff'] == q['completions'] == 0 and q['task_capacity'] > 0, '最终队列空')
    check(w['busy'] == w['failed'] == 0 and w['succeeded'] == 4, '业务错误仍正常execute返回')
    for family, count, failure in [('search', 3, 1), ('suggest', 1, 0)]:
        values = numbers(family)
        check(values['count'] == count and values['failures'] == failure
              and sum(values['buckets']) == count and values['sum_us'] >= values['max_us']
              and not values['collection_failed'], '服务延迟/失败/互斥桶')
    cache = numbers('cache')
    check(cache['hits'] == 1 and cache['misses'] == cache['entries'] == 2 and cache['evictions'] == 0, '精确缓存')
    check(numbers('index') == dict(documents=3, terms=3, recommendation_terms=5), '原创快照元数据')
    stop = numbers('shutdown')
    check(stop['started'] and stop['finished'] and stop['drain_timeouts'] == 0, '关闭屏障最终样本')
    check(b'PRIVATE-QUERY' not in capture.read_bytes() and str(parent).encode() not in capture.read_bytes(), '指标无正文/路径')


def main():
    root = Path(tempfile.mkdtemp(prefix='siftwing-metrics-process-'))
    print('测试现场：', root, flush=True)
    parent, _, _ = ref.golden(root)
    for mode in ('info', 'off', 'warn', 'full'):
        run(root, parent, mode)
    shutil.rmtree(root)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('server', 'indexer', 'writer'):
        parser.add_argument('--' + name, required=True)
    OPTIONS = parser.parse_args()
    ref.OPTIONS = OPTIONS
    main()
