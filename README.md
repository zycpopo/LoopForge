# TcpServer —— 基于 epoll 的高性能 TCP / HTTP 服务器框架（C++11）

一个从零实现的 **Reactor 反应堆模型**网络服务器框架，仅依赖 C++11 标准库与 Linux 系统调用（epoll、timerfd、eventfd、socket），无任何第三方库。代码风格参考 Muduo，结构清晰，适合学习网络编程、事件驱动模型与 HTTP 协议解析。

---

## 目录结构

```
TcpServer/
├── README.md
└── source/                     # 全部源代码
    ├── server.hpp              # 核心网络库（单头文件，~1158 行）
    ├── echo/                   # 演示一：TCP 回显服务器
    │   ├── echo.hpp            # EchoServer 业务封装
    │   ├── main.cc             # 入口（端口 8500）
    │   ├── Makefile            # g++ -std=c++11 -lpthread
    │   └── main                # 已编译好的可执行文件
    └── http/                   # 演示二：HTTP 服务器
        ├── http.hpp            # HTTP 协议层 + HttpServer 路由/静态资源（~846 行）
        ├── main.cc             # 入口（端口 8085）
        ├── Makefile
        ├── main                # 已编译好的可执行文件
        ├── mime                # MIME 类型映射数据（供 http.hpp 参考）
        ├── statu               # HTTP 状态码描述映射数据（供参考）
        └── wwwroot/            # 静态资源根目录
            └── index.html      # 示例页面（登录表单）
```

说明：根目录下没有 `source/server.hpp` 的实际副本，`source/server.hpp` 与 `server.hpp` 是同一文件的两处副本，`echo/` 与 `http/` 均通过 `#include "../server.hpp"` 引用。

---

## 特性

- **Reactor 事件驱动**：单 Reactor + 多线程，主线程监听，从属线程池处理 IO。
- **epoll 多路复用**：基于 `epoll_ctl` / `epoll_wait` 的事件监控与管理。
- **多线程模型**：主 EventLoop（baseloop）负责 accept，实例化的 `LoopThread` 线程池负责连接读写，通过 `eventfd` 唤醒与任务队列实现跨线程任务投递。
- **非阻塞 IO**：所有套接字非阻塞，搭配 Channel 事件回调驱动读写。
- **时间轮定时器（TimerWheel）**：基于 `timerfd` 与环形表盘实现，支持定时任务、非活跃连接超时释放、连接活跃度刷新。
- **缓冲 Buffer**：`vector` 实现的可扩容读写缓冲，支持读偏移/写偏移、确保可写空间、按行读取。
- **完整 HTTP/1.1 解析**：请求行、请求头、正文状态机解析，支持 `GET/HEAD/POST/PUT/DELETE`。
- **HTTP 路由**：基于 `std::regex` 正则的路由匹配，支持动态路径（如 `/numbers/(\d+)`）。
- **静态资源服务**：支持 MIME 类型识别、路径有效性校验（防目录穿越 `..`）、默认 `index.html`、HEAD / GET 请求。
- **保持 / 短连接**：根据 `Connection` 头自动判断 keep-alive 或 close。
- **轻量日志**：宏级别的日志输出（INF / DBG / ERR），带时间戳与线程信息。

---

## 核心架构解析

`server.hpp` 是一个面向对象设计的完整的网络库，核心组件分层如下：

### 1. 底层工具与基础组件

| 组件 | 职责 |
| --- | --- |
| `Buffer` | 自动扩容的读写缓冲区，使用 `vector<char>` 管理内存，维护读写偏移 |
| `Socket` | 套接字封装：create / bind / listen / connect / accpet / recv / send / 非阻塞设置 |
| `Any` | 类型安全的值容器，用于存储连接上下文（如 `HttpContext`） |
| `Util` | 工具类（位于 `http.hpp`）：URL 编解码、文件读写、目录判断、路径校验等 |

### 2. 事件模型

```
Channel  ── 描述符(文件描述符) + 事件回调的绑定（读/写/错误/关闭/任意事件）
Poller   ── 对 epoll 的直接封装（epoll_create、ctl、wait）
EventLoop ── 事件循环：事件监控 -> 就绪事件处理 -> 执行任务池任务
```

- `Channel` 管理一个 fd 的事件注册与回调；`Update()/Remove()` 会委托给所属 `EventLoop`。
- `EventLoop` 每轮循环：`Poller::Poll` 收集活跃 Channel → 逐个 `HandleEvent()` → 执行跨线程投递的任务队列。
- 通过 `eventfd` + `QueueInLoop` 实现跨线程安全的任务投递与 `epoll_wait` 的阻塞唤醒。

### 3. 定时器

- `TimerTask`：一个定时任务（带 ID、延迟时间、取消标志）。
- `TimerWheel`：时间轮（环形表盘，容量 60），基于 `timerfd` 每秒触发一次 `RunTimerTask`，配合 `weak_ptr` 自动释放到期任务。
- 对外提供 `TimerAdd`（新增）、`TimerRefresh`（延迟/刷新活跃度）、`TimerCancel`（取消），所有操作经 `RunInLoop` 在当前线程安全执行。

### 4. 线程池

- `LoopThread`：一个持有独立 `EventLoop` 的工作线程，通过互斥锁+条件变量同步 `loop` 初始化。
- `LoopThreadPool`：维护一组 LoopThread，采用循环（round-robin）方式把新连接分配到不同的 EventLoop，实现多线程负载均衡。

### 5. 连接与连接管理

- `Connection`（`enable_shared_from_this`）：一条 TCP 连接，内含 `Socket + Channel + 输入/输出 Buffer + Any 上下文 + 四个业务回调`，并支持协议升级（`Upgrade`）、非活跃释放（`EnableInactiveRelease`）。
- 状态机：`DISCONNECTED → CONNECTING → CONNECTED → DISCONNECTING`。
- 事件回调驱动内部行为：
  - `HandleRead`：非阻塞读 → 写入输入缓冲 → 调用 message 回调。
  - `HandleWrite`：发送输出缓冲数据 → 无数据时关闭写监控 → 若待关闭则释放。
  - `HandleClose/HandleError`：处理挂断/出错 → 释放连接。
  - `HandleEvent`：刷新活跃度 + 触发任意事件回调。

### 6. 服务器的组装

- `Acceptor`：监听套接字的封装，读事件触发时 `accept` 新连接并回调。
- `TcpServer`：核心服务器类，负责：
  - 维护所有活跃连接（`unordered_map<id, shared_ptr<Connection>>`）；
  - 构造新连接并分配到线程池某个 EventLoop；
  - 对外暴露回调接口：`SetConnectedCallback`、`SetMessageCallback`、`SetClosedCallback`、`SetAnyEventCallback`、`SetThreadCount`、`EnableInactiveRelease`、`RunAfter`。

---

## HTTP 层（`http.hpp`）解析

在 `TcpServer` 之上封装了完整的 HTTP 协议：

- `HttpRequest`：请求方法、路径、版本、头部表、查询参数表、正文、正则匹配结果。
- `HttpResponse`：状态码、响应头、正文，支持 `SetContent`、`SetRedirect`。
- `HttpContext`：逐段的请求解析状态机。
  - 状态：`RECV_HTTP_ERROR → RECV_HTTP_LINE → RECV_HTTP_HEAD → RECV_HTTP_BODY → RECV_HTTP_OVER`。
  - `RecvHttpRequest` 采用 **不 break 的 switch**，一行处理完立即接着处理后续，直到请求完整。
- `HttpServer`：
  - 4 张路由表（GET/HEAD、POST、PUT、DELETE），`std::regex` 正则匹配。
  - 静态资源服务（`IsFileHandler` + `FileHandler`，基于 `_basedir`）。
  - 错误响应页、状态码描述、MIME 类型映射、长短连接判断、keep-alive。

---

## 编译与运行

> 该项目使用 Linux 系统调用（`epoll`、`eventfd`、`timerfd`、`fork` 相关头文件），**只能在 Linux 上编译运行**。

### 编译

每个演示工程自带 Makefile，进入对应目录执行 `make` 即可：

```bash
# 编译 echo 服务
cd source/echo
make                      # 生成可执行文件 main

# 编译 http 服务
cd source/http
make                      # 生成可执行文件 main
```

> 仓库中已包含 `source/echo/main` 与 `source/http/main` 两个预编译可执行文件，Linux 下可直接运行。

### 运行

```bash
# Echo 回显服务（端口 8500，2 个工作线程，10s 非活跃释放）
cd source/echo && ./main
# 测试：nc / telnet 连接后向服务器发送数据，原样回显并关闭连接
nc 127.0.0.1 8500

# HTTP 服务（端口 8085，3 个工作线程，wwwroot 作为静态资源根目录）
cd source/http && ./main
# 浏览器访问 http://127.0.0.1:8085/ 或 curl 测试
curl http://127.0.0.1:8085/
```

---

## 应用扩展（如何自定义业务）

### 自定义 TCP 业务

继承 `TcpServer` 的回调驱动方式，参考 `echo/echo.hpp`：

```cpp
#include "../server.hpp"

class MyServer {
private:
    TcpServer _server;
    void OnMessage(const PtrConnection &conn, Buffer *buf) {
        // 业务处理：从 buf 读数据，通过 conn->Send 回写
    }
public:
    MyServer(int port) : _server(port) {
        _server.SetThreadCount(4);
        _server.SetMessageCallback(std::bind(&MyServer::OnMessage, this,
            std::placeholders::_1, std::placeholders::_2));
    }
    void Start() { _server.Start(); }
};
```

### 自定义 HTTP 路由 / 业务

参考 `http/main.cc`，注册正则路由与处理函数：

```cpp
// GET /hello 返回请求信息
server.Get("/hello", [](const HttpRequest &req, HttpResponse *rsp) {
    rsp->SetContent("Hello, world!", "text/plain");
});

// 支持正则动态路径，匹配 /numbers/12345
server.Get("/numbers/(\\d+)", [](const HttpRequest &req, HttpResponse *rsp) {
    std::string num = req._matches[1];   // 提取括号内捕获的内容
    rsp->SetContent(num, "text/plain");
});

// PUT 上传：将请求正文写入静态目录
server.Put("/upload", [](const HttpRequest &req, HttpResponse *rsp) {
    Util::WriteFile("wwwroot/" + req._path, req._body);
});
```

---

## 技术要点与可改进方向

- **多线程安全性**：对跨线程的定时器操作、连接状态变更均通过 `RunInLoop` / `QueueInLoop` 投递到对应线程执行，避免加锁。
- **避免 SIGPIPE**：`NetWork` 构造时忽略 `SIGPIPE` 信号，防止对端关闭时写入导致进程被信号杀死。
- **连接生命周期**：`Connection` 使用 `shared_ptr` 管理，配合 `shared_from_this` 在回调中安全自持有；服务器持 `_conns` 表与定时器双保险释放。

可能的可改进点：无连接级限流/流量控制、正文未做 `chunked` 解码、无 SSL/TLS 支持、静态资源未做缓存/大文件流式传输、日志未分级文件输出等，可根据需要进一步完善。

---

## 环境要求

- Linux（含 glibc / POSIX 头文件）
- g++ 支持 C++11（`-std=c++11`）
- `make`

---

## 许可证

本项目为学习用途的示例网络库，无特定开源许可证，仅供学习参考。
