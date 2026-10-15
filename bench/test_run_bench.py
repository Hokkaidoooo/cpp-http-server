#!/usr/bin/env python3
"""run_bench.py 的离线单测：解析、统计与报告渲染。

这些用例不需要 Linux、不需要 MongoDB、不需要网络，因此在任何开发机上
都能跑；真实的端到端测量由 CI 的 Linux runner 承担。
"""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_bench import (  # noqa: E402
    BenchError,
    build_configs,
    effective_requests,
    parse_hey,
    parse_listening,
    parse_metrics,
    render_markdown,
    summarize,
)

# 真实 hey 输出（对本地替身服务 2000 请求 / 50 并发），保留原始制表符。
HEY_OUTPUT = """\nSummary:
  Total:\t0.8085 secs
  Slowest:\t0.0370 secs
  Fastest:\t0.0066 secs
  Average:\t0.0199 secs
  Requests/sec:\t2473.6280

  Total data:\t26000 bytes
  Size/request:\t13 bytes

Latency distribution:
  10% in 0.0152 secs
  25% in 0.0158 secs
  50% in 0.0207 secs
  75% in 0.0232 secs
  90% in 0.0244 secs
  95% in 0.0262 secs
  99% in 0.0345 secs

Details (average, fastest, slowest):
  DNS+dialup:\t0.0005 secs, 0.0066 secs, 0.0370 secs
  DNS-lookup:\t0.0000 secs, 0.0000 secs, 0.0000 secs
  req write:\t0.0000 secs, 0.0000 secs, 0.0016 secs
  resp wait:\t0.0193 secs, 0.0003 secs, 0.0362 secs
  resp read:\t0.0000 secs, 0.0000 secs, 0.0011 secs

Status code distribution:
  [200]\t2000 responses

"""

HEY_OUTPUT_WITH_ERRORS = """\nSummary:
  Total:\t1.0000 secs
  Slowest:\t0.9000 secs
  Fastest:\t0.0001 secs
  Average:\t0.0100 secs
  Requests/sec:\t990.0000

Latency distribution:
  50% in 0.0080 secs
  99% in 0.0500 secs

Error distribution:
  [3]\tGet "http://127.0.0.1:18080/": EOF

Status code distribution:
  [200]\t987 responses

"""

METRICS_LINE = ('METRICS {"accepted":36012,"closed":36012,"responses":36000,"dispatched":36000,'
                '"queue_rejected":0,"connection_rejected":0,"expired":0,"stale_completions":0,'
                '"max_active":64,"active":0}')

# 高并发打垮对照服务时的真实输出：有效样本不足，99% 分位退化成 0%。
HEY_OUTPUT_DEGENERATE = """\nSummary:
  Total:\t12.4845 secs
  Slowest:\t2.4696 secs
  Fastest:\t1.2867 secs
  Average:\t2.1574 secs
  Requests/sec:\t30.7582

Latency distribution:
  10% in 1.7373 secs
  25% in 2.2330 secs
  50% in 2.2890 secs
  75% in 2.4332 secs
  90% in 2.4585 secs
  95% in 2.4645 secs
  0% in 0.0000 secs

Status code distribution:
  [200]\t92 responses

Error distribution:
  [292]\tGet "http://127.0.0.1:18085/": dial tcp: connectex refused

"""


def make_entry(workers, concurrency, rps, p50, p99, status=None):
    run = {
        "rps": rps,
        "total_secs": 1.0,
        "p50_ms": p50,
        "p99_ms": p99,
        "latency": {"50": p50, "99": p99},
        "status": status or {"200": 1000},
        "errors": {},
    }
    return {
        "label": f"workers={workers} concurrency={concurrency}",
        "workers": workers,
        "concurrency": concurrency,
        "requests_per_run": 1000,
        "listening": {"port": "18080", "workers": str(workers), "queue": "64"},
        "summary": summarize([run]),
        "runs": [run],
    }


class ParseHeyTests(unittest.TestCase):
    def test_reads_throughput_and_percentiles(self):
        parsed = parse_hey(HEY_OUTPUT)
        self.assertAlmostEqual(parsed["rps"], 2473.6280, places=4)
        self.assertAlmostEqual(parsed["total_secs"], 0.8085, places=4)
        # hey 以秒报告延迟，解析层统一换算为毫秒。
        self.assertAlmostEqual(parsed["p50_ms"], 20.7, places=4)
        self.assertAlmostEqual(parsed["p99_ms"], 34.5, places=4)
        self.assertEqual(parsed["status"], {"200": 2000})
        self.assertEqual(sum(parsed["status"].values()), 2000)
        self.assertEqual(parsed["errors"], {})

    def test_reads_error_distribution(self):
        parsed = parse_hey(HEY_OUTPUT_WITH_ERRORS)
        self.assertEqual(sum(parsed["status"].values()), 987)
        self.assertEqual(sum(parsed["errors"].values()), 3)

    def test_missing_throughput_is_an_error(self):
        with self.assertRaises(BenchError):
            parse_hey("Summary:\n  Total:\t1.0 secs\n")

    def test_missing_percentile_is_an_error(self):
        broken = HEY_OUTPUT.replace("  99% in 0.0345 secs\n", "")
        with self.assertRaises(BenchError):
            parse_hey(broken)

    def test_degenerate_placeholder_gives_null_p99(self):
        # 有效样本极少时 hey 打印 "0% in 0.0000 secs" 顶替 99% 分位。
        parsed = parse_hey(HEY_OUTPUT_DEGENERATE)
        self.assertIsNone(parsed["p99_ms"])
        self.assertAlmostEqual(parsed["p50_ms"], 2289.0, places=4)
        self.assertNotIn("0", parsed["latency"])
        self.assertEqual(sum(parsed["errors"].values()), 292)

    def test_null_p99_without_errors_is_an_error(self):
        inconsistent = HEY_OUTPUT_DEGENERATE.replace(
            'Error distribution:\n  [292]\tGet "http://127.0.0.1:18085/": dial tcp: connectex refused\n\n', "")
        with self.assertRaises(BenchError):
            parse_hey(inconsistent)


class ParseServerLogTests(unittest.TestCase):
    def test_listening_line(self):
        text = "启动检查通过\nLISTENING port=18080 workers=4 queue=64\n"
        self.assertEqual(parse_listening(text),
                         {"port": "18080", "workers": "4", "queue": "64"})

    def test_metrics_uses_last_line(self):
        text = METRICS_LINE + "\n" + METRICS_LINE.replace("36012", "99")
        self.assertEqual(parse_metrics(text)["accepted"], 99)

    def test_missing_metrics_returns_none(self):
        self.assertIsNone(parse_metrics("LISTENING port=1 workers=1 queue=1\n"))

    def test_malformed_metrics_is_an_error(self):
        with self.assertRaises(BenchError):
            parse_metrics('METRICS {"accepted":1,\n')


class SummarizeTests(unittest.TestCase):
    def test_median_and_ranges(self):
        runs = [
            {"rps": 100.0, "p50_ms": 1.0, "p99_ms": 5.0, "status": {"200": 10}, "errors": {}},
            {"rps": 300.0, "p50_ms": 3.0, "p99_ms": 9.0, "status": {"200": 10}, "errors": {}},
            {"rps": 200.0, "p50_ms": 2.0, "p99_ms": 7.0, "status": {"200": 10}, "errors": {}},
        ]
        summary = summarize(runs)
        self.assertEqual(summary["rps_median"], 200.0)
        self.assertEqual(summary["rps_min"], 100.0)
        self.assertEqual(summary["rps_max"], 300.0)
        self.assertEqual(summary["p99_ms_max"], 9.0)
        self.assertEqual(summary["responses_total"], 30)

    def test_empty_is_an_error(self):
        with self.assertRaises(BenchError):
            summarize([])


class ConfigTests(unittest.TestCase):
    def test_worker_sweep_and_concurrency_sweep_share_the_focus_point(self):
        configs = build_configs([1, 2, 4, 8], [1, 16, 64, 128], 4, 64)
        pairs = {(c.workers, c.concurrency) for c in configs}
        self.assertEqual(pairs, {(1, 64), (2, 64), (4, 64), (8, 64), (4, 1), (4, 16), (4, 128)})
        # 焦点组合只出现一次，避免同一次测量被重复计入两个扫描。
        self.assertEqual(len(configs), 7)

    def test_order_is_readable_regardless_of_input_order(self):
        configs = build_configs([4, 1, 8, 2], [64, 1, 128, 16], 4, 64)
        self.assertEqual([(c.workers, c.concurrency) for c in configs],
                         [(1, 64), (2, 64), (4, 64), (8, 64), (4, 1), (4, 16), (4, 128)])


class ReportTests(unittest.TestCase):
    def report(self):
        return {
            "generated_at": "2026-10-15T00:00:00Z",
            "commit": "deadbeef",
            "environment": {"cpu_count": 2, "platform": "Linux-test", "python": "3.10.9",
                            "hey_command": "hey"},
            "loadgen": "hey",
            "requests_per_run": 1000,
            "reps": 3,
            "focus": {"workers": 4, "concurrency": 64},
            "server": {
                "binary": "build/myserver",
                "worker_sweep": [make_entry(1, 64, 1000.0, 1.0, 2.0),
                                 make_entry(4, 64, 4000.0, 0.5, 1.0)],
                "concurrency_sweep": [make_entry(4, 1, 900.0, 1.1, 1.5),
                                      make_entry(4, 64, 4000.0, 0.5, 1.0)],
                "metrics": [{"server_key": "w4", "workers": 4,
                             "metrics": json.loads(METRICS_LINE[len("METRICS "):])}],
            },
            "baseline": {"command": "python3 -m http.server {port}",
                         "requests_per_run": 1000,
                         "sweep": [make_entry(4, 64, 250.0, 8.0, 20.0)]},
        }

    def test_markdown_contains_every_section(self):
        text = render_markdown(self.report())
        for needle in ("# GET / 对比基准", "## 工作线程扫描", "## 并发扫描", "## 对照",
                       "## 收尾统计", "## 边界"):
            self.assertIn(needle, text)
        self.assertIn("4000", text)
        self.assertIn("accepted=36012", text)

    def test_non_200_responses_are_surfaced(self):
        report = self.report()
        report["server"]["worker_sweep"][0]["summary"]["status_total"] = {"200": 990, "503": 10}
        text = render_markdown(report)
        self.assertIn("| 10 |", text)


class EffectiveRequestsTests(unittest.TestCase):
    def test_aligns_down_to_concurrency_multiple(self):
        # hey 会丢弃 -n 除以 -c 的余数；对齐后报告里的请求数才是真实发出的数量。
        self.assertEqual(effective_requests(1200, 64), 1152)
        self.assertEqual(effective_requests(1200, 16), 1200)
        self.assertEqual(effective_requests(1280, 64), 1280)

    def test_fewer_requests_than_concurrency_is_an_error(self):
        with self.assertRaises(BenchError):
            effective_requests(10, 64)


if __name__ == "__main__":
    unittest.main()
