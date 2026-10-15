#!/usr/bin/env python3
"""本地替身服务：严格模拟目标服务器的协议行为，用于离线验证基准脚本。

它不是被测对象，也不是第二套服务实现。它只做一件事：让 run_bench.py 的
进程管理、就绪探测、请求计数和报告生成能在没有 Linux epoll 的机器上跑通。

与真实服务一致的行为：
- 请求行版本必须是 HTTP/1.1，否则返回 505。
- 只接受 GET 和 POST。
- 响应 "Hello, World!"（13 字节），带 Connection: close 后关闭连接。
- 每个连接只处理一个请求。
- 收到 SIGTERM 时打印 LISTENING/METRICS 风格的收尾行，便于验证解析。

用法：python fake_target.py <port> [workers]
"""

import signal
import socket
import sys
import threading

BODY = b"Hello, World!"
RESPONSE = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
    b"Connection: close\r\n\r\n" + BODY
)

state = {"accepted": 0, "closed": 0, "responses": 0}
lock = threading.Lock()
stop = threading.Event()


def handle(connection):
    with lock:
        state["accepted"] += 1
    try:
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 65536:
            chunk = connection.recv(4096)
            if not chunk:
                break
            data += chunk
        if not data:
            return
        line = data.split(b"\r\n", 1)[0].decode("latin-1")
        parts = line.split(" ")
        if len(parts) != 3 or parts[2] != "HTTP/1.1":
            connection.sendall(b"HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        if parts[0] not in ("GET", "POST"):
            connection.sendall(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        connection.sendall(RESPONSE)
        with lock:
            state["responses"] += 1
    finally:
        connection.close()
        with lock:
            state["closed"] += 1


def main():
    port = int(sys.argv[1])
    workers = int(sys.argv[2]) if len(sys.argv) > 2 else 4

    def shutdown(_signum, _frame):
        stop.set()
        print("METRICS " + '{"accepted":%d,"closed":%d,"responses":%d,"dispatched":%d,'
              '"queue_rejected":0,"connection_rejected":0,"expired":0,"stale_completions":0,'
              '"max_active":0,"active":0}' % (
                  state["accepted"], state["closed"], state["responses"], state["responses"]),
              flush=True)
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(512)
    print("LISTENING port=%d workers=%d queue=64" % (port, workers), flush=True)
    while not stop.is_set():
        try:
            connection, _ = listener.accept()
        except OSError:
            break
        threading.Thread(target=handle, args=(connection,), daemon=True).start()


if __name__ == "__main__":
    main()
