#!/usr/bin/env python3
"""GET / 对比基准：在同一台机器、同一次运行内比较配置差异。

设计取舍：
- 服务端每连接只处理一个请求并主动关闭，因此一次请求就是一个完整的
  connect -> 请求 -> 响应 -> close 周期，吞吐即"完整连接周期/秒"。
- 绝对吞吐随 runner 波动，跨机器不可比。本基准只回答两个相对问题：
  增加工作线程是否提升吞吐；并发升到何处开始饱和。
- 负载生成器与被测服务同机共享 CPU，高并发下测得的是两者联合上限，
  不是服务端单机上限，报告里明确标注。

任何解析失败、服务器提前退出、连接未回收，都抛 BenchError 并以非零退出，
不把部分结果写成通过。
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shlex
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

HELLO = b"Hello, World!"


class BenchError(RuntimeError):
    """测量失败。调用方必须让它冒泡，不做兜底。"""


# ---------------------------------------------------------------------------
# 解析与统计：纯函数，可离线单测
# ---------------------------------------------------------------------------

def _require(pattern: str, text: str, name: str) -> str:
    found = re.search(pattern, text, re.MULTILINE)
    if not found:
        raise BenchError(f"缺少字段 {name}")
    return found.group(1)


def parse_hey(text: str) -> dict:
    """解析 hey 的摘要输出。缺字段直接报错，不用默认值掩盖。

    有效样本极少时 hey 会打印 "0% in 0.0000 secs" 这种退化占位，并按不出
    99% 分位；这时 p99 记为 None（报告里显示 n/a），但只有在同时没有任何
    错误时才视为测量不一致——有错误说明样本本来就不足。
    """
    rps = float(_require(r"^\s*Requests/sec:\s+([\d.]+)", text, "Requests/sec"))
    total = float(_require(r"^\s*Total:\s+([\d.]+) secs", text, "Total"))

    status: dict[str, int] = {}
    block = re.search(r"Status code distribution:\n(.*?)(?:\n\s*\n|\Z)", text, re.DOTALL)
    if block:
        for code, count in re.findall(r"^\s*\[(\d+)\]\s+(\d+) responses", block.group(1), re.MULTILINE):
            status[code] = int(count)

    errors: dict[str, int] = {}
    block = re.search(r"Error distribution:\n(.*?)(?:\n\s*\n|\Z)", text, re.DOTALL)
    if block:
        for count, message in re.findall(r"^\s*\[(\d+)\]\s+(.*)$", block.group(1), re.MULTILINE):
            errors[message.strip()] = int(count)

    latency: dict[int, float] = {}
    block = re.search(r"Latency distribution:\n(.*?)(?:\n\s*\n|\Z)", text, re.DOTALL)
    if block:
        for pct, value in re.findall(r"^\s*(\d+)% in ([\d.]+) secs", block.group(1), re.MULTILINE):
            if int(pct) > 0:
                latency[int(pct)] = float(value) * 1000.0
    if 50 not in latency:
        raise BenchError("hey 输出缺少 50% 延迟分位：有效样本不足")
    if 99 not in latency and not errors:
        raise BenchError("hey 输出缺少 99% 延迟分位，且没有任何错误，测量结果不一致")

    return {
        "rps": rps,
        "total_secs": total,
        "p50_ms": latency[50],
        "p99_ms": latency.get(99),
        "latency": {str(k): v for k, v in sorted(latency.items())},
        "status": status,
        "errors": errors,
    }


def parse_metrics(text: str) -> dict | None:
    """取最后一行 METRICS（收尾统计）。没有则返回 None，由调用方决定是否失败。"""
    payload = None
    for line in text.splitlines():
        if line.startswith("METRICS "):
            payload = line[len("METRICS "):]
    if payload is None:
        return None
    try:
        return json.loads(payload)
    except json.JSONDecodeError as error:
        raise BenchError(f"METRICS 行不是合法 JSON：{error}") from error


def parse_listening(text: str) -> dict | None:
    for line in reversed(text.splitlines()):
        if line.startswith("LISTENING "):
            return dict(part.split("=", 1) for part in line[len("LISTENING "):].split())
    return None


def _median_or_none(values) -> float | None:
    present = [value for value in values if value is not None]
    return statistics.median(present) if present else None


def _max_or_none(values) -> float | None:
    present = [value for value in values if value is not None]
    return max(present) if present else None


def summarize(runs: list[dict]) -> dict:
    if not runs:
        raise BenchError("没有任何一次测量结果")
    return {
        "runs": len(runs),
        "rps_median": statistics.median(r["rps"] for r in runs),
        "rps_min": min(r["rps"] for r in runs),
        "rps_max": max(r["rps"] for r in runs),
        "p50_ms_median": statistics.median(r["p50_ms"] for r in runs),
        "p99_ms_median": _median_or_none(r["p99_ms"] for r in runs),
        "p99_ms_max": _max_or_none(r["p99_ms"] for r in runs),
        "responses_total": sum(sum(r["status"].values()) for r in runs),
        "error_total": sum(sum(r["errors"].values()) for r in runs),
        "status_total": _merge_status(runs),
    }


def _merge_status(runs: list[dict]) -> dict:
    merged: dict[str, int] = {}
    for run in runs:
        for code, count in run["status"].items():
            merged[code] = merged.get(code, 0) + count
    return dict(sorted(merged.items()))


# ---------------------------------------------------------------------------
# 进程与网络
# ---------------------------------------------------------------------------

def probe(port: int, timeout: float = 3.0) -> bytes | None:
    """发一次真实 HTTP/1.1 请求，返回响应字节；连不上返回 None。"""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
            sock.sendall(b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
            sock.settimeout(timeout)
            chunks = []
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                chunks.append(data)
            return b"".join(chunks)
    except OSError:
        return None


def _tail(path: Path, lines: int = 20) -> str:
    try:
        return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])
    except OSError:
        return "(无日志)"


def wait_ready(process: subprocess.Popen, log_path: Path, port: int, expect_hello: bool,
               timeout: float = 45.0) -> dict | None:
    deadline = time.monotonic() + timeout
    listening = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise BenchError(f"服务器提前退出（码 {process.returncode}）：\n{_tail(log_path)}")
        listening = parse_listening(log_path.read_text(encoding="utf-8", errors="replace")) or listening
        response = probe(port)
        if response is not None and (not expect_hello or HELLO in response):
            return listening
        time.sleep(0.2)
    raise BenchError(f"{timeout:.0f} 秒内未就绪：\n{_tail(log_path)}")


def stop_process(process: subprocess.Popen, handle, log_path: Path, timeout: float = 20.0) -> str:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise BenchError(f"服务器未在 {timeout:.0f} 秒内响应终止信号：\n{_tail(log_path)}")
    handle.flush()
    handle.close()
    return log_path.read_text(encoding="utf-8", errors="replace")


def effective_requests(requests: int, concurrency: int) -> int:
    """hey 把 -n 平均分给 -c 个 worker，除不尽的余数会被静默丢弃。

    这里显式对齐到并发数的整数倍，并把实际发出的请求数记进报告，
    否则报告的"请求数"与真实发出的请求数会不一致。
    """
    aligned = (requests // concurrency) * concurrency
    if aligned == 0:
        raise BenchError(f"请求数 {requests} 少于并发 {concurrency}，hey 不会发出任何请求")
    return aligned


def run_hey(hey: str, url: str, requests: int, concurrency: int, timeout: float) -> dict:
    command = [*hey.split(), "-n", str(requests), "-c", str(concurrency), "-disable-keepalive", url]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise BenchError(f"hey 超时（{timeout:.0f} 秒，{requests} 请求 / {concurrency} 并发）") from error
    if completed.returncode != 0:
        raise BenchError(f"hey 退出码 {completed.returncode}：\n{completed.stderr.strip()}")
    parsed = parse_hey(completed.stdout)
    responded = sum(parsed["status"].values()) + sum(parsed["errors"].values())
    if responded != requests:
        raise BenchError(
            f"响应数 {sum(parsed['status'].values())} 加错误数 {sum(parsed['errors'].values())} "
            f"不等于请求数 {requests}，本次测量不完整")
    return parsed


# ---------------------------------------------------------------------------
# 目标与测量
# ---------------------------------------------------------------------------

@dataclass
class Config:
    server_key: str
    workers: int
    concurrency: int

    @property
    def label(self) -> str:
        return f"workers={self.workers} concurrency={self.concurrency}"


@dataclass
class Target:
    name: str
    command: list
    env: dict = field(default_factory=dict)
    expect_metrics: bool = False
    requests: int = 12000
    expect_hello: bool = False
    sets_workers: bool = False


def build_configs(workers: list[int], concurrency: list[int], focus_workers: int,
                  focus_concurrency: int) -> list[Config]:
    """按"工作线程从少到多、并发从小到大"排列，让执行日志和报告顺序可读。"""
    configs = [Config(f"w{w}", w, focus_concurrency) for w in sorted(workers)]
    configs += [Config(f"w{focus_workers}", focus_workers, c) for c in sorted(concurrency)
                if c != focus_concurrency]
    return configs


def measure_target(target: Target, configs: list[Config], args) -> tuple[list[dict], list[dict]]:
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    port = args.port
    url = f"http://127.0.0.1:{port}/"

    groups: dict[str, list[Config]] = {}
    for config in configs:
        groups.setdefault(config.server_key, []).append(config)

    results: list[dict] = []
    metric_records: list[dict] = []

    for key, group in groups.items():
        group = sorted(group, key=lambda config: config.concurrency)
        workers = group[0].workers
        log_path = workdir / f"{target.name}-{key}.log"
        env = dict(target.env)
        if target.sets_workers:
            env["WORKER_THREADS"] = str(workers)
        command = [str(part).format(port=port, workers=workers) for part in target.command]
        print(f"[{target.name}] 启动 {command} (WORKER_THREADS={workers})", flush=True)
        handle = open(log_path, "wb")
        process = subprocess.Popen(command, cwd=args.cwd, env={**os.environ, **env},
                                   stdout=handle, stderr=subprocess.STDOUT)
        try:
            listening = wait_ready(process, log_path, port, target.expect_hello)
            for config in group:
                count = effective_requests(target.requests, config.concurrency)
                runs = []
                for attempt in range(args.reps):
                    runs.append(run_hey(args.hey, url, count, config.concurrency, args.timeout))
                    print(f"[{target.name}] {config.label} 第 {attempt + 1}/{args.reps} 次："
                          f"{runs[-1]['rps']:.1f} req/s", flush=True)
                results.append({
                    "label": config.label,
                    "workers": config.workers,
                    "concurrency": config.concurrency,
                    "requests_per_run": count,
                    "listening": listening,
                    "summary": summarize(runs),
                    "runs": runs,
                })
        finally:
            log_text = stop_process(process, handle, log_path)
        metrics = parse_metrics(log_text)
        if target.expect_metrics and metrics is None:
            raise BenchError(f"[{target.name}] {key} 未输出 METRICS 收尾统计：\n{_tail(log_path)}")
        if metrics is not None:
            if metrics.get("accepted") != metrics.get("closed"):
                raise BenchError(f"[{target.name}] {key} 连接未回收：accepted={metrics.get('accepted')} "
                                 f"closed={metrics.get('closed')}")
            if metrics.get("active") != 0:
                raise BenchError(f"[{target.name}] {key} 退出时仍有活跃连接：active={metrics.get('active')}")
            metric_records.append({"server_key": key, "workers": workers, "metrics": metrics})
        print(f"[{target.name}] {key} 结束：accepted/closed="
              f"{(metrics or {}).get('accepted', 'n/a')}/{(metrics or {}).get('closed', 'n/a')}", flush=True)

    return results, metric_records


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------

def _fmt(value, digits: int = 2) -> str:
    return "n/a" if value is None else f"{value:.{digits}f}"


def _table(entries: list[dict]) -> list[str]:
    lines = [
        "| 配置 | 吞吐 req/s（中位） | 波动区间 | P50 ms | P99 ms | 非 200 响应 | 错误 |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for entry in entries:
        summary = entry["summary"]
        non_200 = sum(count for code, count in summary["status_total"].items() if code != "200")
        lines.append(
            f"| {entry['label']} | {summary['rps_median']:.0f} | "
            f"{summary['rps_min']:.0f} ~ {summary['rps_max']:.0f} | "
            f"{_fmt(summary['p50_ms_median'])} | {_fmt(summary['p99_ms_median'])} | "
            f"{non_200} | {summary['error_total']} |")
    return lines


def render_markdown(report: dict) -> str:
    env = report["environment"]
    lines = [
        "# GET / 对比基准",
        "",
        f"- 生成时间（UTC）：{report['generated_at']}",
        f"- 提交：`{report['commit']}`",
        f"- 运行环境：{env['platform']}，{env['cpu_count']} 逻辑核",
        f"- 负载生成器：`{report['loadgen']}`，每次测量 {report['requests_per_run']} 请求，"
        f"重复 {report['reps']} 次取中位",
        "- 请求形式：HTTP/1.1 `GET /`，`Connection: close`，每连接一个请求",
        "",
        "## 工作线程扫描",
        "",
        f"并发固定为 {report['focus']['concurrency']}，改变工作线程数：",
        "",
        *_table(report["server"]["worker_sweep"]),
        "",
        "## 并发扫描",
        "",
        f"工作线程固定为 {report['focus']['workers']}，改变并发连接数：",
        "",
        *_table(report["server"]["concurrency_sweep"]),
    ]
    if report.get("baseline"):
        baseline = report["baseline"]
        lines += [
            "",
            "## 对照",
            "",
            f"同一 runner、同一负载生成器下运行 `{baseline['command']}`：",
            "",
            *_table(baseline["sweep"]),
        ]
    lines += [
        "",
        "## 收尾统计",
        "",
    ]
    if not report["server"]["metrics"]:
        lines.append("- 目标未输出 METRICS 收尾统计，本次没有连接回收数据。")
    for record in report["server"]["metrics"]:
        metrics = record["metrics"]
        lines.append(f"- `{record['server_key']}`：accepted={metrics['accepted']}，"
                     f"closed={metrics['closed']}，active={metrics['active']}，"
                     f"max_active={metrics['max_active']}，queue_rejected={metrics['queue_rejected']}，"
                     f"connection_rejected={metrics['connection_rejected']}")
    lines += [
        "",
        "## 边界",
        "",
        "- 绝对吞吐随 CI runner 型号与负载波动，跨机器、跨时间不可直接比较；"
        "只有同一次运行内的相对关系可读。",
        "- 负载生成器与被测服务共享同一台机器的 CPU，高并发下测得的是两者联合上限，"
        "不是服务端单机上限。",
        "- 服务端不支持 keep-alive，每次请求都包含完整的 TCP 建立与关闭，"
        "因此该数字衡量的是连接生命周期吞吐，不衡量长连接场景下的响应能力。",
    ]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="被测服务器可执行文件")
    parser.add_argument("--baseline", help="对照服务器命令模板，可含 {port}")
    parser.add_argument("--hey", default="hey")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--requests", type=int, default=12000)
    parser.add_argument("--baseline-requests", type=int, default=3000)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--workers", default="1,2,4,8")
    parser.add_argument("--concurrency", default="1,16,64,128")
    parser.add_argument("--focus-workers", type=int, default=4)
    parser.add_argument("--focus-concurrency", type=int, default=64)
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--cwd", default=".")
    parser.add_argument("--workdir", default="build-bench")
    parser.add_argument("--out-json", default="bench/results/bench.json")
    parser.add_argument("--out-md", default="bench/results/bench.md")
    parser.add_argument("--expect-metrics", action="store_true")
    parser.add_argument("--timeout", type=float, default=180.0, help="单次 hey 的超时秒数")
    return parser.parse_args(argv)


def environment(args) -> dict:
    return {
        "cpu_count": os.cpu_count(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "hey_command": args.hey,
    }


def git_commit(cwd: str) -> str:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=cwd, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def main(argv=None) -> int:
    args = parse_args(argv)
    if not args.binary and not args.baseline:
        print("必须给出 --binary 或 --baseline", file=sys.stderr)
        return 2

    workers = [int(value) for value in args.workers.split(",")]
    concurrency = [int(value) for value in args.concurrency.split(",")]
    env = dict(pair.split("=", 1) for pair in args.env)
    configs = build_configs(workers, concurrency, args.focus_workers, args.focus_concurrency)

    report = {
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": git_commit(args.cwd),
        "environment": environment(args),
        "loadgen": args.hey,
        "requests_per_run": args.requests,
        "reps": args.reps,
        "focus": {"workers": args.focus_workers, "concurrency": args.focus_concurrency},
    }

    if args.binary:
        target = Target(name="server", command=[*shlex.split(args.binary), "{port}"], env=env,
                        expect_metrics=args.expect_metrics, requests=args.requests,
                        expect_hello=True, sets_workers=True)
        results, metrics = measure_target(target, configs, args)
        report["server"] = {
            "binary": args.binary,
            "worker_sweep": [r for r in results if r["concurrency"] == args.focus_concurrency],
            "concurrency_sweep": [r for r in results if r["workers"] == args.focus_workers],
            "metrics": metrics,
        }

    if args.baseline:
        baseline_configs = [Config("baseline", args.focus_workers, c) for c in concurrency]
        target = Target(name="baseline", command=shlex.split(args.baseline),
                        requests=args.baseline_requests, expect_hello=False)
        results, _ = measure_target(target, baseline_configs, args)
        report["baseline"] = {
            "command": args.baseline,
            "requests_per_run": args.baseline_requests,
            "sweep": results,
        }

    out_json = Path(args.out_json)
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    Path(args.out_md).write_text(render_markdown(report), encoding="utf-8")
    print(f"报告写入 {out_json} 与 {args.out_md}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
