# M1 —— 仿 muduo 的 C++ 网络库

## 1. 项目是什么

一个从零实现的 Reactor 网络库，用于学习 Linux 网络编程与 C++ 后端开发。
只依赖 pthread 和 Linux 系统调用，不引入任何第三方网络库。

目前完成的是基础设施层与网络层：

- **one loop per thread**：每个 IO 线程一个 `EventLoop` + 一个 `epoll`
- **epoll LT** 事件分发，`epoll_event.data.ptr` 直接存 `Channel*`
- **eventfd** 跨线程唤醒，**timerfd** 驱动定时器
- `Acceptor` / `TcpServer` / `TcpConnection` / `TcpClient` / `Connector`
- 应用层 `Buffer`：三索引 + `readv` 分散读
- 非阻塞 connect、指数退避重连、半关闭、EMFILE 兜底、`sendfile` 零拷贝

规模（2026-10-22）：

| 目录 | 文件 | 行数 |
|---|---:|---:|
| `muduo/base/` | 17 | 942 |
| `muduo/net/` | 38 | 3589 |
| `tests/` | 18 | 3493 |

另有 131 个单元测试、1 个 echo 集成测试脚本（6 项断言）。

## 2. 架构图

```
┌──────────────────────────────────────────────────────┐
│ examples/echo_server.cc                              │
│   用户代码：EventLoop + Acceptor + Channel + Buffer   │
└───────────────────────────┬──────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────┐
│ net/   网络层（Reactor）                              │
│                                                      │
│   Acceptor ──► TcpServer ──► TcpConnection            │
│      │              │              │                 │
│      └────────► Channel ◄──────────┘                 │
│                    │                                 │
│               EventLoop ──── EpollPoller              │
│                    │                                 │
│    TimerQueue(timerfd)   EventLoopThread(Pool)       │
│    Connector ──► TcpClient                           │
└───────────────────────────┬──────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────┐
│ base/   基础设施（与网络无关）                        │
│   Thread / ThreadPool / Mutex / Condition / Atomic   │
│   Timestamp / CurrentThread / Logging / BlockingQueue│
└──────────────────────────────────────────────────────┘
```

一次读事件的数据流：

```
epoll_wait
  → EpollPoller::fillActiveChannels
  → Channel::handleEvent
  → TcpConnection::handleRead
  → Buffer::readFd
  → MessageCallback
```

线程模型：

```
主线程（baseLoop）
  ├─ Acceptor 监听 listen fd
  └─ 轮询把新连接分给 IO 线程
IO 线程 × N
  └─ 每个线程一个 EventLoop，负责各自连接上的读写
```

## 3. 模块表

### base/

| 文件 | 作用 |
|---|---|
| `noncopyable.h` / `Types.h` | 禁止拷贝基类、公共类型与 `implicit_cast` |
| `Atomic.h` | 基于 `__atomic_*` 的原子整数 |
| `Mutex.h` / `Condition.h` | 互斥锁、条件变量（RAII + 持锁线程记录） |
| `CountDownLatch.h` | 倒计时门闩，用于等待线程就绪 |
| `Thread.h/.cc` | 线程封装：命名、启动同步、异常安全析构 |
| `ThreadPool.h/.cc` | 业务线程池（无界任务队列） |
| `BlockingQueue.h` | 通用阻塞队列工具 |
| `Timestamp.h/.cc` | 微秒时间戳与格式化 |
| `CurrentThread.h/.cc` | 线程局部缓存（tid、线程名） |
| `Logging.h/.cc` | 同步日志：时间、TID、级别、errno |

### net/

| 文件 | 作用 |
|---|---|
| `SocketsOps.h/.cc` | socket/bind/listen/accept4 等系统调用薄封装 |
| `Socket.h/.cc` | RAII 管理 fd，析构自动 close |
| `InetAddress.h/.cc` | `sockaddr_in` 封装与 IP:Port 转换 |
| `Endian.h` | 主机序 / 网络序转换 |
| `Channel.h/.cc` | 一个 fd 的事件与回调（Reactor 核心抽象） |
| `Poller.h/.cc` / `EpollPoller.h/.cc` | IO 多路复用抽象与 epoll 实现 |
| `EventLoop.h/.cc` | 事件循环：poll、跨线程任务、定时器、唤醒 |
| `EventLoopThread.h/.cc` | 启动一个线程并在其中运行 EventLoop |
| `EventLoopThreadPool.h/.cc` | IO 线程池，轮询分配连接 |
| `Timer.h/.cc` / `TimerId.h` / `TimerQueue.h/.cc` | timerfd 定时器与取消 |
| `Buffer.h/.cc` | 应用层缓冲，处理粘包/半包 |
| `Callbacks.h/.cc` | 回调类型与默认回调 |
| `Acceptor.h/.cc` | 监听并接受新连接（含 EMFILE 兜底） |
| `Connector.h/.cc` | 非阻塞 connect + 指数退避重连 |
| `TcpConnection.h/.cc` | 连接状态机、读写路径、半关闭、sendfile |
| `TcpServer.h/.cc` | 服务端：Acceptor + 线程池 + 连接表 |
| `TcpClient.h/.cc` | 客户端：Connector + 单连接管理 |

### 其他

| 文件 | 作用 |
|---|---|
| `examples/echo_server.cc` | echo 服务器（手写 Acceptor + Channel，用于验证底层组件） |
| `tests/test_*.cc` | GoogleTest 单元测试，共 131 个用例 |
| `tests/integration/test_echo.sh` | echo 集成测试（基础回显、并发、1MB 大流量、存活检查） |

## 4. 怎么构建

环境：Linux / WSL2、g++（C++17）、CMake ≥ 3.10、GoogleTest。

```bash
cd ~/learn/M1

# 常规构建
cmake -S . -B build
cmake --build build -j4
```

产物：

```
build/unit_tests     # 单元测试
build/echo_server    # echo 示例
```

Sanitizer 构建（ASan 与 TSan 必须分目录）：

```bash
# 越界 / use-after-free / 泄漏 / 未定义行为
cmake -S . -B build-asan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build-asan -j4

# 数据竞争
cmake -S . -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan -j4
```

新增源文件后需要把它加进 `CMakeLists.txt` 的 `MUDUO_SRCS`；
新增测试文件需要加进 `unit_tests` 的源文件列表。

## 5. 怎么测

```bash
cd ~/learn/M1

# 全部单元测试
./build/unit_tests

# 只跑一组 / 一个用例
./build/unit_tests --gtest_filter='BufferTest.*'
./build/unit_tests --gtest_filter='TcpConnectionTest.SendMultiOverIovMax'

# 列出所有用例
./build/unit_tests --gtest_list_tests

# ctest：每个用例独立进程，单项超时 60 秒
ctest --test-dir build --output-on-failure

# 集成测试（参数：服务器路径、端口）
bash tests/integration/test_echo.sh ./build/echo_server 18888

# Sanitizer
./build-asan/unit_tests
./build-tsan/unit_tests
```

测试约定：

- 用例名用 `Suite.Case`，名字要说明"测什么、期望什么"
- 网络代码用 `socketpair` 造连接，fd 必须自己设 `O_NONBLOCK`
- 所有可能阻塞的地方加超时（`SO_RCVTIMEO` / `timeout` / ctest 的 60 秒）
- 写完用例后做一次**反向验证**：故意改坏实现，确认测试真的会失败

## 6. 已知限制

- **没有协议层**：只有 TCP，尚未实现 HTTP/静态文件服务。
- **测试覆盖不均**：`TcpConnection` 只有 2 个用例（sendMulti、sendfile 交错）；
  `TcpServer` / `TcpClient` / `Connector` 还没有单元测试。
- **echo 示例没走 TcpServer**：`examples/echo_server.cc` 仍手写
  `Acceptor + Channel`，用于单独验证底层组件。
- **没有优雅退出**：未实现 `SignalHandler`（signalfd），进程收到 SIGINT/SIGTERM 直接退出。
- **没有压测数据**：尚无压测客户端与 QPS/延迟统计。
- **仅支持 IPv4**：`InetAddress` 只封装 `sockaddr_in`。
- **无连接级超时**：没有 idle timeout / 读写超时，慢连接会一直占用 fd。
- **sendfile 的降级路径吃内存**：输出缓冲非空时降级为 `pread` 整个文件再 append，
  大文件请求会占用与文件等量的用户态内存。
- **日志是同步写 stdout**：没有异步双缓冲、文件滚动。
- **EventLoop 不可重启**：`quit()` 之后 `quit_` 保持为 1，不支持再次 `loop()`。
- **只支持 LT**：没有实现 ET 模式。
