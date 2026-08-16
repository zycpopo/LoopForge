# LoopForge

基于 C++11和Linux Reactor 模型的轻量级 C++ 网络库

项目使用 Reactor 事件驱动模型，底层基于 `epoll`、`eventfd`、`timerfd` 和非阻塞 Socket，实现 TCP 连接管理、事件循环、线程池、定时器和简单的 HTTP 服务示例。

## 目录结构

~~~text
TcpServer/
├── README.md
├── include/                  # 拆分的核心头文件
│   ├── server.hpp            # 拆分版本统一入口
│   ├── server_common.hpp     # 公共依赖和日志宏
│   ├── buffer.hpp            # 缓冲区
│   ├── socket.hpp            # Socket 封装
│   ├── channel.hpp           # 文件描述符事件
│   ├── poller.hpp            # epoll 封装
│   ├── timer.hpp             # TimerTask 和 TimerWheel
│   ├── event_loop.hpp        # 事件循环
│   ├── thread_pool.hpp       # 工作线程池
│   ├── any.hpp               # 上下文容器
│   ├── connection.hpp        # TCP 连接
│   ├── acceptor.hpp          # 监听器
│   ├── tcp_server.hpp        # TCP 服务器
│   └── network.hpp           # 网络初始化
└── source/
    ├── server.hpp            # 原始单头文件
    ├── echo/                 # TCP 回显示例
    │   ├── echo.hpp
    │   ├── main.cc
    │   └── Makefile
    └── http/                 # HTTP 服务示例
        ├── http.hpp
        ├── main.cc
        ├── Makefile
        └── wwwroot/
~~~

## 主要功能

- 基于 `epoll` 的事件驱动 I/O；
- 非阻塞 TCP Socket；
- 主线程监听新连接，工作线程处理连接事件；
- 支持连接建立、消息、关闭和任意事件回调；
- 支持跨线程任务投递；
- 支持定时任务和非活跃连接释放；
- 提供可扩容的读写缓冲区；
- 提供简单的 HTTP 请求解析和路由；
- 支持 GET、HEAD、POST、PUT、DELETE；
- 支持静态文件和 MIME 类型识别。

## 环境要求

项目依赖 Linux 系统调用，不能直接在 Windows 原生环境编译。

需要：

- Linux 或配置好的 WSL；
- 支持 C++11以上 的 g++；
- make；
- pthread、epoll、eventfd、timerfd 等 Linux/POSIX 接口。

## 使用拆分后的核心头文件

新代码推荐使用 `include/server.hpp`：

~~~cpp
#include "server.hpp"
~~~

编译时将 `include` 加入头文件搜索路径：

~~~bash
g++ -std=c++11 main.cc -I./include -o server -lpthread
~~~

如果直接从项目根目录编译，也可以这样写：

~~~cpp
#include "include/server.hpp"
~~~

~~~bash
g++ -std=c++11 main.cc -I. -o server -lpthread
~~~

不要在同一个源文件中同时包含 `source/server.hpp` 和 `include/server.hpp`，两者都会定义相同的核心类。

## TCP 服务基本用法

下面是一个简单的回显服务器：

~~~cpp
#include "server.hpp"

class EchoServer {
private:
    TcpServer _server;

    void OnMessage(const PtrConnection &conn, Buffer *buffer) {
        const size_t size = buffer->ReadAbleSize();

        conn->Send(buffer->ReadPosition(), size);
        buffer->MoveReadOffset(size);
    }

public:
    explicit EchoServer(int port)
        : _server(port) {
        _server.SetThreadCount(2);
        _server.SetMessageCallback(
            std::bind(&EchoServer::OnMessage, this,
                      std::placeholders::_1,
                      std::placeholders::_2));
    }

    void Start() {
        _server.Start();
    }
};

int main() {
    EchoServer server(9000);
    server.Start();
}
~~~

消息回调处理完数据后，需要调用 `MoveReadOffset` 移动读偏移，否则同一段数据可能会被重复处理。

## 常用 API

### TcpServer

~~~cpp
TcpServer(int port);

void SetThreadCount(int count);
void SetConnectedCallback(...);
void SetMessageCallback(...);
void SetClosedCallback(...);
void SetAnyEventCallback(...);

void EnableInactiveRelease(int seconds);
void RunAfter(const std::function<void()> &task, int delay);
void Start();
~~~

`SetThreadCount(0)` 表示不创建额外工作线程，连接直接使用主 EventLoop。

### Connection

~~~cpp
int Fd();
int Id();
bool Connected();

void Send(const char *data, size_t len);
void Shutdown();
void Release();

void SetContext(const Any &context);
Any *GetContext();

void EnableInactiveRelease(int seconds);
void CancelInactiveRelease();
~~~

`Send` 会将数据复制到发送缓冲区，然后由连接所属的 EventLoop 异步发送。

### Buffer

~~~cpp
void Write(const void *data, uint64_t len);
void WriteAndPush(const void *data, uint64_t len);

uint64_t ReadAbleSize();
char *ReadPosition();
void MoveReadOffset(uint64_t len);

std::string ReadAsString(uint64_t len);
std::string ReadAsStringAndPop(uint64_t len);
std::string GetLine();
std::string GetLineAndPop();

void Clear();
~~~

## HTTP 服务

HTTP 相关代码位于 `source/http/http.hpp`，它是在 `TcpServer` 基础上实现的示例层。

主要类型：

- `HttpRequest`：保存请求方法、路径、请求头、查询参数和正文；
- `HttpResponse`：设置状态码、响应头、正文和重定向；
- `HttpContext`：解析 HTTP 请求；
- `HttpServer`：管理 HTTP 路由和静态资源。

基本用法：

~~~cpp
#include "http.hpp"

void Hello(const HttpRequest &req, HttpResponse *rsp) {
    rsp->SetContent("Hello, world!", "text/plain");
}

int main() {
    HttpServer server(8085);
    server.Get("/hello", Hello);
    server.Listen();
}
~~~

也可以使用正则表达式注册路由：

~~~cpp
server.Get("/user/(\\d+)",
    [](const HttpRequest &req, HttpResponse *rsp) {
        rsp->SetContent(req._matches[1], "text/plain");
    });
~~~

常用响应接口：

~~~cpp
rsp->SetHeader("Content-Type", "text/plain");
rsp->SetContent("response body", "text/plain");
rsp->SetRedirect("/login");
~~~

设置静态文件目录：

~~~cpp
server.SetBaseDir("./wwwroot/");
~~~

## 编译和运行

### 编译 Echo 示例

~~~bash
cd source/echo
make
~~~

运行：

~~~bash
./main
~~~

测试：

~~~bash
nc 127.0.0.1 8500
~~~

Echo 示例默认监听 8500 端口，使用两个工作线程，并启用 10 秒非活跃连接释放。

### 编译 HTTP 示例

~~~bash
cd source/http
make
~~~

运行：

~~~bash
./main
~~~

测试：

~~~bash
curl http://127.0.0.1:8085/
curl http://127.0.0.1:8085/hello
curl -X POST -d "username=demo&password=demo" \
     http://127.0.0.1:8085/login
~~~

HTTP 示例默认监听 8085 端口，使用三个工作线程，并将当前目录下的 `wwwroot` 作为静态资源目录。

## 运行流程

一次 TCP 连接的大致处理流程：

1. Acceptor 接受新连接；
2. TcpServer 创建 Connection；
3. Connection 被分配到某个 EventLoop；
4. Poller 通过 epoll 等待文件描述符事件；
5. Channel 根据事件调用读写或关闭回调；
6. Connection 读取数据并调用业务回调；
7. 业务代码处理 Buffer，并通过 Connection 发送响应；
8. 连接关闭或超时后释放。

## 注意事项

- 项目只能在 Linux/POSIX 环境使用；
- `Start()` 会进入持续运行的事件循环；
- 业务回调通常在 EventLoop 所在线程执行；
- 多线程共享业务数据时，需要自行保证线程安全；
- HTTP 请求正文依赖 `Content-Length`；
- 当前没有实现 chunked 传输；
- 当前没有 TLS/SSL；
- 静态文件会整体读入内存，不适合直接用于大文件服务；
- 时间轮容量固定为 60；
- 当前项目没有自动化测试和 CI 配置；
- 使用前建议先编译两个示例并进行基本 TCP/HTTP 测试。




