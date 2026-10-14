# C++ HTTP 服务器

Linux 下的 C++ HTTP 服务器：epoll 事件循环 + 线程池 + 路由 + MongoDB 用户系统 + Nginx 反向代理 + Docker 部署，附带登录、注册与图片上传页面。

## 结构

| 文件 | 作用 |
| --- | --- |
| `HttpServer.h` | epoll 事件循环：接受连接、读请求、发送响应，维护每个连接的状态 |
| `ThreadPool.h` | 固定大小线程池（`std::packaged_task` + 条件变量） |
| `Router.h` | 路由表：注册、登录、上传、图片列表接口 |
| `HttpRequest.h` / `HttpResponse.h` | 请求解析（请求行、请求头、表单与 multipart 文件）与响应构造 |
| `Database.h` | MongoDB 封装：用户注册/登录、图片元信息存取 |
| `Logger.h` | 加锁的文件日志（`LOG_INFO` / `LOG_WARNING` / `LOG_ERROR`） |
| `UI/` | 登录、注册、上传页面 |
| `Dockerfile`、`docker-compose.yml`、`nginx.conf` | 容器化部署：应用 + MongoDB + Nginx |

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/register` | 表单注册（username / password） |
| POST | `/login` | 表单登录 |
| POST | `/upload` | multipart 上传图片：文件保存到 `images/`，元信息写入 MongoDB |
| GET | `/images` | 返回已上传图片的 JSON 列表 |

## 运行

依赖 MongoDB C++ 驱动（r3.9.0，安装脚本见 `mongo1.sh`）。在 Linux 环境中：

```sh
# 1. 安装 MongoDB C++ 驱动
# 2. 编译
g++ -std=c++17 main.cpp -o myserver -lmongocxx -lbsoncxx
# 3. 运行（默认端口 8080）
./myserver 8080
```

`main.cpp` 里的 MongoDB 连接串默认是 `mongodb://172.20.0.2:27017`，需要改成实际地址。

用 Docker 部署时，`docker-compose.yml` 会启动 MongoDB 与应用容器；Nginx 按 `nginx.conf` 反向代理到应用（配置里给出了三个上游和权重，可按需调整）。

## 说明

- `mongo.sh` 等脚本中的容器名、镜像名请按本机环境修改。
- 代码与配置保持完成时的原貌，仅供学习参考。
