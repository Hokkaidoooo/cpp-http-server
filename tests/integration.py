"""真实 MongoDB、HTTP 字节序列、上传、过载与资源回收验收；不使用替身服务器。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import os
from pathlib import Path
import random
import re
import socket
import statistics
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import urlencode
import uuid
import zlib

from pymongo import MongoClient

ROOT = Path(__file__).resolve().parents[1]
PASSWORD = "Correct+horse-battery-2026"


def png_fixture(width=64, height=64):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    source = random.Random(20261015)
    pixels = b"".join(b"\0" + bytes(source.randrange(256) for _ in range(width * 3)) for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def multipart(name, data, mime="image/png", description="测试图片"):
    boundary = "cpp_test_boundary_20261015"
    prefix = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
              f'Content-Type: {mime}\r\n\r\n').encode()
    suffix = (f'\r\n--{boundary}\r\nContent-Disposition: form-data; name="description"\r\n\r\n').encode()
    description = description.encode() if isinstance(description, str) else description
    return prefix + data + suffix + description + f"\r\n--{boundary}--\r\n".encode(), f"multipart/form-data; boundary={boundary}"


class Harness:
    def __init__(self, binary, output):
        self.binary, self.output = binary, output
        self.temporary = tempfile.TemporaryDirectory(prefix="cpp-http-integration-")
        self.storage = Path(self.temporary.name) / "images"
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        self.database_name = "cpp_http_test_" + uuid.uuid4().hex
        self.client = MongoClient(os.environ["MONGODB_URI"])
        self.client.admin.command("ping")
        self.database = self.client[self.database_name]
        self.env = dict(os.environ, MONGODB_DATABASE=self.database_name, PORT=str(self.port),
                        BIND_ADDRESS="127.0.0.1", APP_ROOT=str(ROOT), STORAGE_ROOT=str(self.storage),
                        SESSION_COOKIE_SECURE="0", SESSION_TTL_SECONDS="300", WORKER_THREADS="4",
                        TASK_QUEUE_CAPACITY="16", MAX_CONNECTIONS="256", MAX_UPLOAD_BYTES="65536",
                        MAX_IMAGE_PIXELS="65536", READ_TIMEOUT_MS="2000", WRITE_TIMEOUT_MS="2000",
                        PUBLIC_ORIGIN=f"http://127.0.0.1:{self.port}")
        self.log = []
        self.measurements = {}
        self.process = subprocess.Popen([str(binary)], cwd=ROOT, env=self.env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, encoding="utf-8")
        self.reader = threading.Thread(target=self._read_log, daemon=True)
        self.reader.start()
        deadline = time.monotonic() + 15
        while not any(line.startswith("LISTENING ") for line in self.log):
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("服务器未完成真实启动：" + "".join(self.log))
            time.sleep(0.02)

    def _read_log(self):
        self.log.extend(self.process.stdout)

    def request(self, method, path, body=b"", cookie=None, content_type=None, headers=None):
        fields = dict(headers or {})
        if cookie:
            fields["Cookie"] = cookie
        if content_type:
            fields["Content-Type"] = content_type
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        try:
            connection.request(method, path, body=body, headers=fields)
            response = connection.getresponse()
            data = response.read()
            return response.status, dict((k.lower(), v) for k, v in response.getheaders()), data
        finally:
            connection.close()

    def register(self, username=None):
        username = username or "u_" + uuid.uuid4().hex[:20]
        result = self.request("POST", "/register", urlencode({"username": username, "password": PASSWORD}).encode(),
                              content_type="application/x-www-form-urlencoded")
        if result[0] != 201:
            raise AssertionError(f"注册失败：{result}")
        return username

    def login(self, username):
        result = self.request("POST", "/login", urlencode({"username": username, "password": PASSWORD}).encode(),
                              content_type="application/x-www-form-urlencoded")
        if result[0] != 303:
            raise AssertionError(f"登录失败：{result}")
        return result[1]["set-cookie"].split(";", 1)[0]

    def socket(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=10)

    @staticmethod
    def response_bytes(connection):
        connection.settimeout(20)
        chunks = []
        while True:
            data = connection.recv(65536)
            if not data:
                break
            chunks.append(data)
        result = b"".join(chunks)
        head, body = result.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        headers = dict(line.lower().split(":", 1) for line in lines[1:])
        if len(body) != int(headers["content-length"]):
            raise AssertionError("响应正文长度不一致")
        if headers["connection"].strip() != "close":
            raise AssertionError("连接关闭契约缺失")
        return int(lines[0].split(" ")[1]), body

    def raw(self, data, half_close=False):
        with self.socket() as connection:
            connection.sendall(data)
            if half_close:
                connection.shutdown(socket.SHUT_WR)
            return self.response_bytes(connection)

    def resources(self):
        base = Path("/proc") / str(self.process.pid)
        status = dict(line.split(":", 1) for line in (base / "status").read_text().splitlines() if ":" in line)
        return {"fds": len(list((base / "fd").iterdir())), "threads": int(status["Threads"]),
                "rss_kib": int(status["VmRSS"].strip().split()[0])}

    def close(self):
        errors = []
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                errors.append("服务器未在 30 秒内完成退出")
        self.reader.join(timeout=5)
        if self.process.returncode != 0:
            errors.append(f"服务器退出码为 {self.process.returncode}")
        log = "".join(self.log)
        (self.output.parent / "server.log").write_text(log, encoding="utf-8")
        if re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:", log):
            errors.append("Sanitizer 检测到错误，请查 server.log")
        summaries = [line[8:] for line in self.log if line.startswith("METRICS ")]
        if summaries:
            self.measurements["server"] = json.loads(summaries[-1])
            metrics = self.measurements["server"]
            if metrics["accepted"] != metrics["closed"] or metrics["active"] != 0:
                errors.append("退出时连接回收计数不一致")
        else:
            errors.append("缺少服务器退出指标")
        if not self.database_name.startswith("cpp_http_test_"):
            raise RuntimeError("拒绝清理非本轮测试数据库")
        try:
            self.client.drop_database(self.database_name)
        except Exception as error:
            errors.append(f"本轮测试数据库清理失败：{error}")
        finally:
            self.client.close()
            self.temporary.cleanup()
        return errors


class Integration(unittest.TestCase):
    harness = None

    @property
    def h(self):
        return self.harness

    def test_01_home_and_pages(self):
        self.assertEqual(self.h.request("GET", "/")[::2], (200, b"Hello, World!"))
        for path in ("/login", "/register"):
            response = self.h.request("GET", path)
            self.assertEqual(response[0], 200)
            self.assertIn(b"<form", response[2])
        for path in ("/upload", "/images"):
            self.assertEqual(self.h.request("GET", path)[0], 401)
        self.assertEqual(self.h.request("POST", "/upload", b"invalid")[0], 401)

    def test_02_partial_body_no_early_response_or_duplicate(self):
        username = "u_" + uuid.uuid4().hex[:20]
        body = urlencode({"username": username, "password": PASSWORD}).encode()
        header = (f"POST /register HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                  f"Content-Length: {len(body)}\r\n\r\n").encode()
        with self.h.socket() as connection:
            connection.sendall(header + body[:13])
            connection.settimeout(0.15)
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(body[13:])
            self.assertEqual(self.h.response_bytes(connection)[0], 201)
        records = list(self.h.database.users.find({"username": username}))
        self.assertEqual(len(records), 1)
        self.assertNotIn("password", records[0])
        self.assertTrue(records[0]["password_hash"].startswith("$argon2id$"))
        self.assertNotIn(PASSWORD, records[0]["password_hash"])

    def test_03_partial_large_header(self):
        wire = b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: " + b"x" * 6000 + b"\r\n\r\n"
        with self.h.socket() as connection:
            connection.sendall(wire[:2000])
            connection.settimeout(0.1)
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(wire[2000:])
            self.assertEqual(self.h.response_bytes(connection), (200, b"Hello, World!"))

    def test_04_protocol_failures(self):
        prefix = b"POST /login HTTP/1.1\r\nHost: localhost\r\n"
        cases = [(b"Content-Length: -2\r\n\r\n", 400), (b"Content-Length: abc\r\n\r\n", 400),
                 (b"Content-Length: 1\r\nContent-Length: 1\r\n\r\nx", 400),
                 (b"Content-Length: 9000000\r\n\r\n", 413),
                 (b"Transfer-Encoding: chunked\r\n\r\n", 501),
                 (b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400),
                 (b"Expect: 100-continue\r\nContent-Length: 0\r\n\r\n", 417), (b"\r\n", 411)]
        for suffix, status in cases:
            with self.subTest(suffix=suffix):
                self.assertEqual(self.h.raw(prefix + suffix)[0], status)
        self.assertEqual(self.h.raw(prefix + b"Content-Length: 30\r\n\r\nshort", half_close=True)[0], 400)
        self.assertEqual(self.h.raw(b"GET / HTTP/1.1\r\nHost: a\r\nX: " + b"a" * 17000 + b"\r\n\r\n")[0], 431)
        self.assertEqual(self.h.raw(b"GET / HTTP/1.0\r\nHost: a\r\n\r\n")[0], 505)

    def test_05_invalid_configuration_and_database(self):
        environments = []
        missing = dict(self.h.env)
        missing.pop("MONGODB_URI")
        environments.append(missing)
        environments.append(dict(self.h.env, PORT="0"))
        environments.append(dict(self.h.env, MONGODB_URI="mongodb://127.0.0.1:9/?serverSelectionTimeoutMS=500&connectTimeoutMS=500"))
        for env in environments:
            result = subprocess.run([str(self.h.binary)], cwd=ROOT, env=env, capture_output=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"FATAL", result.stderr)
            self.assertNotIn(b"LISTENING", result.stdout)

    def test_06_login_logout_and_origin(self):
        username = self.h.register()
        cookie = self.h.login(username)
        self.assertEqual(self.h.request("GET", "/upload", cookie=cookie)[0], 200)
        wrong = urlencode({"username": username, "password": "another-wrong-password"}).encode()
        self.assertEqual(self.h.request("POST", "/login", wrong, content_type="application/x-www-form-urlencoded")[0], 401)
        self.assertEqual(self.h.request("POST", "/logout", cookie=cookie, headers={"Origin": "https://other.example"})[0], 403)
        self.assertEqual(self.h.request("GET", "/images", cookie=cookie + "; " + cookie)[0], 400)
        response = self.h.request("POST", "/logout", cookie=cookie)
        self.assertEqual(response[0], 200)
        self.assertIn("HttpOnly", response[1]["set-cookie"])
        self.assertIn("SameSite=Strict", response[1]["set-cookie"])
        self.assertIn("Max-Age=0", response[1]["set-cookie"])
        self.assertEqual(self.h.request("GET", "/upload", cookie=cookie)[0], 401)

    def test_07_upload_read_and_validation(self):
        username = self.h.register()
        cookie = self.h.login(username)
        png = png_fixture()
        self.assertGreater(len(png), 4096)
        body, content_type = multipart("example.png", png, description='中文描述 "引号"')
        response = self.h.request("POST", "/upload", body, cookie, content_type)
        self.assertEqual(response[0], 201, response)
        image = json.loads(response[2])
        self.assertEqual(image["name"], "example.png")
        self.assertEqual(image["description"], '中文描述 "引号"')
        self.assertEqual(self.h.request("GET", image["url"], cookie=cookie)[2], png)
        self.assertEqual(json.loads(self.h.request("GET", "/images", cookie=cookie)[2]), [image])
        other_cookie = self.h.login(self.h.register())
        self.assertEqual(self.h.request("GET", image["url"], cookie=other_cookie)[0], 404)
        self.assertEqual(self.h.request("POST", "/upload", body, cookie, content_type)[0], 409)
        cases = [("../outside.png", png, "image/png", "ok", 400),
                 ("%2e%2e.png", png, "image/png", "ok", 400),
                 ("bad\\name.png", png, "image/png", "ok", 400),
                 ("not.png", b"<script>alert(1)</script>", "image/png", "ok", 415),
                 ("polyglot.png", png + b"<script>x</script>", "image/png", "ok", 415),
                 ("image.txt", png, "image/png", "ok", 415),
                 ("mime.png", png, "text/html", "ok", 415),
                 ("large.png", b"x" * 65537, "image/png", "ok", 413),
                 ("invalid-description.png", png, "image/png", b"\xff", 400)]
        for name, content, mime, description, expected in cases:
            with self.subTest(name=name):
                data, kind = multipart(name, content, mime, description)
                self.assertEqual(self.h.request("POST", "/upload", data, cookie, kind)[0], expected)
        self.assertEqual(self.h.request("GET", "/images/../../secret", cookie=cookie)[0], 400)
        self.assertEqual(self.h.request("GET", image["url"], cookie=cookie)[2], png)
        self.assertFalse(list(self.h.storage.rglob("*.part")))
        owner_dir = self.h.storage / hashlib.blake2b(username.encode(), digest_size=32).hexdigest()
        self.assertEqual(len(list(owner_dir.glob("*.asset"))), 1)

    def test_08_concurrent_same_name_does_not_overwrite(self):
        cookie = self.h.login(self.h.register())
        png = png_fixture(16, 16)
        body, kind = multipart("race.png", png)
        with ThreadPoolExecutor(max_workers=12) as executor:
            statuses = list(executor.map(lambda _: self.h.request("POST", "/upload", body, cookie, kind)[0], range(12)))
        self.assertEqual(statuses.count(201), 1, statuses)
        self.assertEqual(statuses.count(409), 11, statuses)
        self.assertFalse(list(self.h.storage.rglob("*.part")))

    def test_09_concurrency_baseline_and_resources(self):
        for _ in range(32):
            self.assertEqual(self.h.request("GET", "/")[0], 200)
        before = self.h.resources()
        def measured(_):
            started = time.perf_counter()
            result = self.h.request("GET", "/")
            return result[0], result[2], (time.perf_counter() - started) * 1000
        started = time.perf_counter()
        with ThreadPoolExecutor(max_workers=12) as executor:
            results = list(executor.map(measured, range(240)))
        elapsed = time.perf_counter() - started
        latencies = sorted(item[2] for item in results)
        errors = sum(status != 200 or body != b"Hello, World!" for status, body, _ in results)
        deadline = time.monotonic() + 3
        after = self.h.resources()
        while after["fds"] > before["fds"] and time.monotonic() < deadline:
            time.sleep(0.05)
            after = self.h.resources()
        self.h.measurements["baseline"] = {"requests": 240, "concurrency": 12, "errors": errors,
            "elapsed_seconds": elapsed, "latency_ms_mean": statistics.mean(latencies),
            "latency_ms_p50": latencies[119], "latency_ms_p95": latencies[227], "latency_ms_p99": latencies[237],
            "resources_before": before, "resources_after": after, "route": "GET /"}
        self.assertEqual(errors, 0)
        self.assertLessEqual(after["fds"], before["fds"])
        self.assertEqual(after["threads"], before["threads"])

    def test_10_disconnect_and_descriptor_reuse(self):
        for _ in range(100):
            with self.h.socket() as connection:
                connection.sendall(b"POST /login HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\npartial")
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.assertEqual(self.h.request("GET", "/")[::2], (200, b"Hello, World!"))
        self.assertIsNone(self.h.process.poll())

    def test_11_queue_saturation_is_explicit(self):
        names = ["load_" + uuid.uuid4().hex[:20] for _ in range(96)]
        gate = threading.Barrier(96)
        def register(name):
            gate.wait(timeout=20)
            body = urlencode({"username": name, "password": PASSWORD}).encode()
            return self.h.request("POST", "/register", body, content_type="application/x-www-form-urlencoded")[0]
        with ThreadPoolExecutor(max_workers=96) as executor:
            statuses = list(executor.map(register, names))
        self.assertTrue(set(statuses).issubset({201, 503}), statuses)
        self.assertGreater(statuses.count(503), 0, "未实际触发满队列")
        self.assertEqual(self.h.database.users.count_documents({"username": {"$in": names}}), statuses.count(201))
        self.h.measurements["saturation"] = {"connections": 96, "created": statuses.count(201), "rejected_503": statuses.count(503)}
        self.assertEqual(self.h.request("GET", "/")[0], 200)

    def test_12_incomplete_request_timeout(self):
        with self.h.socket() as connection:
            connection.sendall(b"GET / HTTP/1.1\r\nHost:")
            self.assertEqual(self.h.response_bytes(connection)[0], 408)

    def test_13_database_outage_is_not_success(self):
        container = os.environ.get("TEST_MONGO_CONTAINER")
        if not container:
            self.skipTest("未提供隔离测试 MongoDB 容器，未进行运行中停库验收")
        self.assertRegex(container, r"^[a-f0-9]{12,64}$")
        try:
            subprocess.run(["docker", "stop", "--time", "5", container], check=True, capture_output=True)
            body = urlencode({"username": "outage_test", "password": PASSWORD}).encode()
            self.assertEqual(self.h.request("POST", "/login", body, content_type="application/x-www-form-urlencoded")[0], 500)
            self.assertEqual(self.h.request("GET", "/")[0], 200)
        finally:
            subprocess.run(["docker", "start", container], check=True, capture_output=True)
            deadline = time.monotonic() + 30
            while True:
                try:
                    self.h.client.admin.command("ping")
                    break
                except Exception:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    harness = Harness(args.binary.resolve(), output)
    Integration.harness = harness
    result = None
    try:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Integration))
    finally:
        cleanup_errors = harness.close()
        report = {"tests_run": result.testsRun if result else 0,
                  "failures": [(str(test), trace) for test, trace in result.failures] if result else [],
                  "errors": [(str(test), trace) for test, trace in result.errors] if result else [],
                  "skipped": [(str(test), reason) for test, reason in result.skipped] if result else [],
                  "cleanup_errors": cleanup_errors, "measurements": harness.measurements,
                  "database": harness.database_name, "temporary_directory_cleaned": True}
        report["passed"] = bool(result and result.wasSuccessful() and not cleanup_errors and not result.skipped)
        output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"报告：{output}；通过={report['passed']}；测试数={report['tests_run']}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
