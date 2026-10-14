![C++ HTTP 服务器](assets/cover.svg)

# C++ HTTP 服务器

Linux 下的 C++17 HTTP 服务原型。使用 epoll 处理连接事件、线程池执行请求，通过路由完成注册、登录校验、图片上传和图片路径查询，MongoDB 保存用户记录与图片元信息。

适合从一个小型工程中阅读“连接 → 解析 → 路由 → 存储 → 响应”的完整调用链。仓库还保留了 Nginx 与 Docker 配置，但这些配置尚不足以独立构建并启动应用。

## 请求如何经过系统

```text
客户端请求 → HttpServer 的 epoll 事件循环 → ThreadPool
           → HttpRequest 解析 → Router 分发
           → Database / 图片文件 → HttpResponse → 客户端
```

| 阅读入口 | 作用 |
| --- | --- |
| [main.cpp](main.cpp) | 配置端口、数据库连接并启动服务 |
| [HttpServer.h](HttpServer.h) | 连接状态、事件循环、请求读取与响应发送 |
| [ThreadPool.h](ThreadPool.h) | 固定大小线程池，使用任务队列、条件变量与 `std::packaged_task` |
| [HttpRequest.h](HttpRequest.h) / [HttpResponse.h](HttpResponse.h) | 请求行、请求头、表单与 multipart 解析；响应构造 |
| [Router.h](Router.h) | 注册、登录、上传与图片列表路由 |
| [Database.h](Database.h) | 用户记录及图片元信息的 MongoDB 存取 |
| [Logger.h](Logger.h) / [UI/](UI/) | 文件日志与前端页面 |

## 接口

| 方法 | 路径 | 当前行为 |
| --- | --- | --- |
| GET | `/` | 返回 `Hello, World!` 文本 |
| GET | `/login`、`/register`、`/upload` | 登录、注册与上传页面 |
| POST | `/register` | 接收 `username`、`password` 表单字段，保存注册记录 |
| POST | `/login` | 校验用户名和密码并返回文本；没有创建登录会话 |
| POST | `/upload` | 解析 multipart，将文件写到 `images/` 并保存元信息 |
| GET | `/images` | 返回图片保存路径的 JSON 列表 |

`/images` 是列表接口，当前没有配套的 `/images/*` 图片下载路由。

## 运行

需要 Linux、支持 C++17 的编译器、线程库、MongoDB 服务及 MongoDB C/C++ 驱动。仓库脚本引用 C++ 驱动 r3.9.0；[mongo1.sh](mongo1.sh) 的下载步骤已被注释，不能直接视为完整安装器。

先将 [main.cpp](main.cpp) 中的 `mongodb://172.20.0.2:27017` 改为可访问的数据库地址。页面与上传目录使用相对路径，应从仓库根目录运行。

若驱动已经安装，并可由 `pkg-config` 找到 `libmongocxx` 和 `libbsoncxx`，构建命令可写为：

```sh
pkg-config --modversion libmongocxx libbsoncxx
g++ -std=c++17 -pthread main.cpp -o myserver $(pkg-config --cflags --libs libmongocxx libbsoncxx)
mkdir -p images
./myserver 8080
```

这是依赖准备完成后的构建示例，尚未经过干净 Linux 环境验证。驱动安装在非系统目录时，还需按其安装位置配置运行时动态库搜索路径。

### 容器配置现状

- [Dockerfile](Dockerfile) 没有安装 MongoDB C++ 驱动或编译应用；[start.sh](start.sh) 却依赖 `myserver` 已存在。
- [docker-compose.yml](docker-compose.yml) 的 MongoDB 环境变量缩进需要修正，数据库地址也需要与应用统一。
- [nginx.conf](nginx.conf) 列出三个上游端口，启动脚本只启动一个应用实例；登录、注册接口的缓存配置也需要调整。

因此当前不提供“一条 Compose 命令即可运行”的保证。

## 当前边界

- 请求读取尚未完整处理分段到达的请求体与大文件上传，也没有协议兼容性或吞吐基准。
- 注册记录使用明文密码，登录没有会话机制，上传没有登录校验；应仅使用隔离环境和测试账号。
- 上传文件名与保存目录边界需要加固，当前实现不适合接收不受信任的文件。
- 仓库没有自动化测试或 CI；阅读代码及构建成功都不能替代真实 HTTP 请求验证。

后续重点是统一请求读取状态机、补充协议边界测试、完善上传与认证规则，再建立可复现的构建和性能基准。
