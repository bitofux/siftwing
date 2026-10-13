"""独立Python TCP客户端验证真实探针进程，线字节不用生产codec。"""
import pathlib
import select
import socket
import struct
import subprocess
import sys

probe = pathlib.Path(sys.argv[1])
checks = 0
def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise RuntimeError(message)
def frame(kind, request_id, body=b"{}", status=0):
    return struct.pack("!4sBBHHHIQ", b"SFTW", 1, 24, kind, 0, status, len(body), request_id) + body
def exact(peer, length):
    result = b""
    while len(result) < length:
        part = peer.recv(length - len(result))
        require(bool(part), "响应前缀不能被EOF截断")
        result += part
    return result
help_result = subprocess.run([str(probe), "--help"], capture_output=True, timeout=5)
require(help_result.returncode == 0 and b"PING/PONG" in help_result.stdout, "帮助入口")
for arguments in (["-1"], ["65536"], ["0", "0"], ["0", "2", "1001"], ["0", "1", "0", "extra"]):
    result = subprocess.run([str(probe), *arguments], capture_output=True, timeout=5)
    require(result.returncode == 2 and not result.stdout, "非法参数无监听公告")
with open("/dev/full", "wb") as output:
    result = subprocess.run([str(probe), "--help"], stdout=output, stderr=subprocess.PIPE, timeout=5)
    require(result.returncode == 3, "帮助写入失败不得伪成功")
    result = subprocess.run([str(probe), "0", "1", "0"], stdout=output, stderr=subprocess.PIPE, timeout=5)
    require(result.returncode == 3, "端口公告写入失败需释放已创建资源并退出")
process = subprocess.Popen([str(probe), "0", "200", "20"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    require(bool(select.select([process.stdout], [], [], 5)[0]), "有限等待监听公告")
    line = process.stdout.readline()
    require(line.startswith(b"PORT "), "机器可读实际端口")
    port = int(line.split()[1])
    with socket.create_connection(("127.0.0.1", port), timeout=3) as peer:
        peer.settimeout(3)
        request = frame(1, 1)
        peer.sendall(request[:7]); peer.sendall(request[7:] + frame(1, 2))
        require(exact(peer, 52) == frame(0x8001, 1) + frame(0x8001, 2), "分片/粘包连续PONG独立wire")
        peer.sendall(frame(3, 3) + frame(1, 4, b"[]"))
        expected = frame(0xffff, 3, b'{"message":"invalid request"}', 1) + frame(0xffff, 4, b'{"message":"invalid request"}', 1)
        require(exact(peer, len(expected)) == expected, "未接业务及Body拒绝均是受信错误响应")
        peer.sendall(frame(1, 5)); peer.shutdown(socket.SHUT_WR)
        require(exact(peer, 26) == frame(0x8001, 5) and peer.recv(1) == b"", "半关闭响应先于服务端EOF")
    stdout, stderr = process.communicate(timeout=10)
    require(process.returncode == 0 and b"accepted=1 completed=5" in stdout and not stderr, "有限轮次进程正常关闭")
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
print(f"checks={checks} failures=0")
