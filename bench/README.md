# GET / 对比基准

`run_bench.py` 在同一台机器、同一次运行内测量 `GET /` 的吞吐与延迟，用来回答两个问题：

- 增加工作线程，吞吐是否跟着涨，涨到几个线程为止；
- 并发连接升高到多少，吞吐开始饱和。

## 为什么这样测

服务端每连接只处理一个请求并主动关闭，不支持 keep-alive。所以一次请求就是一个完整的
`connect -> 请求 -> 响应 -> close` 周期，报告里的吞吐是"完整连接周期/秒"，衡量的是
连接生命周期吞吐，不是长连接下的请求处理能力。

绝对吞吐随机器型号、邻居负载和调度波动，跨机器、跨时间都不可直接比较。因此这里不做
跨机器排名，只在同一次运行内做相对比较，并且把机器信息一并记录在报告里。

负载生成器与被测服务跑在同一台机器上，共享 CPU。高并发时测到的是"服务端加客户端"的
联合上限，不是服务端单机上限。

## 怎么跑

CI 由 [性能基准工作流](../.github/workflows/benchmark.yml) 在 push 到 `main` 或手动触发时执行，
结果作为 artifact 上传，摘要写进 job summary。

本地运行需要 Linux（服务端依赖 epoll）和可用的 MongoDB：

```sh
go install github.com/rakyll/hey@v0.1.4
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --parallel 2

python3 bench/run_bench.py \
  --binary build-rel/myserver \
  --env "MONGODB_URI=$MONGODB_URI" \
  --env MONGODB_DATABASE=cpp_http_bench \
  --env SESSION_COOKIE_SECURE=0 \
  --expect-metrics
```

`--expect-metrics` 要求服务端在退出时输出 `METRICS` 收尾统计，并断言
`accepted == closed`、`active == 0`。任何一次测量解析不到字段、服务器提前退出、
连接没有回收，脚本都会以非零退出结束，不会把部分结果写成通过。

没有 Linux 环境时，可以先跑解析层的离线单测：

```sh
python3 -m unittest discover -s bench -p "test_*.py"
```

`bench/tests/fake_target.py` 是一个严格模拟本服务协议行为的本地替身（请求行必须是
HTTP/1.1，否则 505；每连接一个请求；响应 `Hello, World!`），用于在没有 epoll 的机器上
验证进程管理、就绪探测和报告生成：

```sh
python3 bench/run_bench.py \
  --binary "python3 bench/tests/fake_target.py" \
  --requests 1200 --reps 2 --workers 1,2,4 --concurrency 1,16,64
```

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--binary` | 无 | 被测服务器；命令模板里 `{port}` 会被替换为端口 |
| `--baseline` | 无 | 可选的对照服务器命令，用同一套扫描测量 |
| `--hey` | `hey` | 负载生成器命令 |
| `--port` | `18080` | 监听端口 |
| `--requests` | `12000` | 每组配置每次发出的请求数 |
| `--reps` | `3` | 每组配置重复次数，取中位数 |
| `--workers` | `1,2,4,8` | 工作线程扫描点 |
| `--concurrency` | `1,16,64,128` | 并发扫描点 |
| `--focus-workers` | `4` | 并发扫描时固定的工作线程数 |
| `--focus-concurrency` | `64` | 工作线程扫描时固定的并发数 |
| `--expect-metrics` | 关 | 要求并校验服务端 `METRICS` 收尾统计 |

## 已知的测量陷阱

- **hey 会丢弃除不尽的余数。** hey 把 `-n` 平均分给 `-c` 个 worker，`-n` 不是 `-c`
  整数倍时，余数不会发出。脚本会显式把请求数对齐到并发数的整数倍，并把实际发出的
  请求数记进报告，避免报告的"请求数"与真实发出的数量不一致。
- **有效样本太少时，hey 用 `0%` 顶替 99% 分位。** 这时 p99 记为 `null`，报告显示
  `n/a`；只有在同时没有任何错误时脚本才判定测量不一致并报错。
- **对照服务要挑。** `python3 -m http.server` 的监听队列只有 5，并发超过十几个就开始
  直接拒连，测到的是队列溢出而不是 HTTP 处理能力，不适合作为高并发对照。

## 输出

- `bench/results/*.json`：完整数据，含每次重复的原始测量、收尾统计和运行环境。
- `bench/results/*.md`：人读的报告，含两张扫描表、收尾统计和边界说明。
