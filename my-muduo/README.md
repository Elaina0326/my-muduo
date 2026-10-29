# my-muduo —— 仿 muduo 的 C++ 网络库

## 1. 项目是什么

一个以 muduo 的架构和 API 为参考、从基础组件开始实现的 Reactor 网络库，
用于学习 Linux 网络编程与 C++ 后端开发。
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

许可证：MIT；架构参考 muduo 并在其代码基础上改写，详见 `LICENSE` 与 `NOTICE`。

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
| `ThreadPool.h/.cc` | 业务线程池（任务队列可设上限，默认无界） |
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
| `examples/echo_server.cc` | echo 服务器（手写 Acceptor + Channel，单线程，用于验证底层组件） |
| `examples/echo_server_mt.cc` | echo 服务器（`TcpServer` + EventLoopThreadPool，可配 IO 线程数，压测用；单连接输出缓冲超 8 MB 主动断开慢消费者） |
| `benchmarks/bench_client.cc` | ping-pong 压测客户端：多线程、TCP_NODELAY、warmup、QPS/P50/P99 |
| `tests/test_*.cc` | GoogleTest 单元测试，共 131 个用例 |
| `tests/integration/test_echo.sh` | echo 集成测试（基础回显、并发、1MB 大流量、存活检查） |

## 4. 怎么构建

环境：Linux / WSL2、g++（C++17）、CMake ≥ 3.10、GoogleTest。

```bash
cd ~/learn/my-muduo

# 常规构建
cmake -S . -B build
cmake --build build -j4
```

产物：

```
build/unit_tests     # 单元测试
build/echo_server    # echo 示例（单线程，手写 Acceptor + Channel）
build/echo_server_mt # echo 示例（多线程，走 TcpServer + EventLoopThreadPool）
build/bench_client   # ping-pong 压测客户端
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
cd ~/learn/my-muduo

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

压测（先构建 Release）：

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j4

# 启动多线程 echo 服务器（端口、IO 线程数；0 表示全部在 base loop 处理）
./build-rel/echo_server_mt 8888 4 &

# host、port、客户端线程数、每连接请求数、负载字节数
./build-rel/bench_client 127.0.0.1 8888 8 20000 64

# 200 条连接（客户端仍是 4 个线程）
#   连接数多时先提高 ulimit -n，例如 ulimit -n 100000
./build-rel/bench_client 127.0.0.1 8888 4 2000 64 --connections 200

# 固定跑 60 秒（duration 模式下 messages_per_conn 被忽略，可传 0）
./build-rel/bench_client 127.0.0.1 8888 4 0 64 --duration 60
```

`host` 只接受 IPv4 字面量（如 `127.0.0.1`），不支持 `localhost` 或域名。

退出码：`0` 全部成功，`1` 参数错误，`2` 部分失败但仍有可用数据，
`3` 完全失败（没有可用连接或没有完成任何请求）。

每条连接独立 connect 并设置 `TCP_NODELAY`；先 warmup 1 秒，再以
ping-pong 方式计时，QPS 按实际耗时计算。输出包含每线程 QPS、
min/avg/P50/P99/P999/max，以及超时、IO 错误和被丢弃的失败轮次数。
延迟样本是一轮 RTT：

- `--connections == --threads`：一轮就是一条消息，标注为 `rtt`；
- 多连接：一轮包含多条消息，标注为 `batch`，**不要和单连接结果横向比较**。

输出会把连接建立成功率、超时和 IO 错误分开统计。

批量测量用 `benchmarks/` 下的三个脚本：

- `run_benchmarks.sh` —— 16 组矩阵（延迟基线 / IO 线程 / 连接数 / 消息大小 / 长稳），原始输出落盘到 `results/<date>/`
- `run_server_scaling.sh` —— 固定客户端配置，只扫服务端 IO 线程数
- `bench_repeat.sh` —— 同条件重复 N 次，取中位数与范围（可设为每次重启服务端）

实测数据、复现命令，以及测量方法上踩过的三个坑，见 [`docs/performance.md`](docs/performance.md)。

> 那份报告里每个 QPS 都绑定了客户端线程数与连接数：同一个服务端，
> 客户端 1 线程与 8 线程能差 39%。**不附客户端配置的 QPS 没有意义。**

测试约定：

- 用例名用 `Suite.Case`，名字要说明"测什么、期望什么"
- 网络代码用 `socketpair` 造连接，fd 必须自己设 `O_NONBLOCK`
- 所有可能阻塞的地方加超时（`SO_RCVTIMEO` / `timeout` / ctest 的 60 秒）
- 写完用例后做一次**反向验证**：故意改坏实现，确认测试真的会失败

## 6. 与 muduo 的差异

> 本项目以 muduo 的架构和 API 为参考，代码在其基础上改写。下面 7 处是与上游
> muduo 有意的差异，按性质分三类。这些判断针对的是本项目的使用场景，
> 不代表 muduo 本身有问题。

### 修正（缺陷）

1. **`SocketsOps::accept` 的错误分类**
   上游对每次 `accept4` 失败都记 `LOG_SYSERR`，未知错误直接终止进程。
   本项目把 errno 分成两组：`EAGAIN / ECONNABORTED / EINTR / EPROTO / EPERM / EMFILE`
   是可恢复错误，静默返回给 `Acceptor`（EMFILE 由预留 idleFd 兜底）；
   `EBADF / EFAULT / EINVAL / ENFILE / ENOBUFS / ENOMEM / ENOTSOCK` 才记日志并 `LOG_FATAL`。
   LT 模式下 EAGAIN/EMFILE 是常态，逐条打日志会在高并发下刷屏。

2. **`EventLoopThread` 析构时的 eventfd 竞态**
   上游析构函数无锁读 `loop_` 再调用 `quit()`；而 loop 线程可能已经跑完
   `loop()`、析构 `EventLoop` 并关闭 `wakeupFd_`，此时 `quit()` 会往已关闭
   （甚至被复用）的 fd 上 `write`。本项目的析构函数在 `mutex_` 保护下读取
   `loop_` 并调用 `quit()`，`threadFunc` 也在同一把锁下清空 `loop_`，
   然后才在锁外 `join()`。

3. **`Timestamp::toFormattedString` 检查 `gmtime_r` 返回值**
   上游忽略返回值；`gmtime_r` 失败时不会写 `tm_time`，后续读取属于未定义行为。
   本项目把 `tm` 零初始化，并在失败时返回带原始值的诊断串
   `invalid(秒.微秒)`。

4. **`ThreadPool` 不让任务异常逃逸**
   上游没有捕获；异常一旦逃出线程入口就会 `std::terminate()`。
   本项目在线程初始化回调和任务执行处分别捕获 `std::exception` 与 `...`，
   记录 `LOG_ERROR` 后继续处理后续任务，不会静默减少 worker。

### 收敛（移除无用能力）

5. **移除未消费的 `EPOLLPRI` 注册**
   上游 `kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP`。`EPOLLPRI` 对应
   TCP 紧急数据（OOB），但本项目没有任何代码读取 OOB；只要对端发 1 字节
   OOB，LT 模式下的 epoll 就会持续报告 `EPOLLPRI`，形成忙循环（一字节 DoS）。
   本项目 `kReadEvent = EPOLLIN`，对端关闭通过 `read()` 返回 0 识别。

### 取舍（API 形态）

6. **`BlockingQueue::take` 返回 `bool`**
   上游是阻塞的 `T take()`，消费者无法被通知退出。本项目改成
   `bool take(T* out)` 并增加 `close()`：队列关闭且为空时返回 `false`，
   消费线程可以干净退出。这是接口形态差异，不是缺陷。

7. **`Atomic` 少一个 `decrement()`**
   上游提供 `decrement()`；本项目只保留 `decrementAndGet()` / `add(-1)` 等
   实际用到的操作，API 面更小。需要时用 `add(-1)` 等价替换。

## 7. 已知限制

- **没有协议层**：只有 TCP，尚未实现 HTTP/静态文件服务。
- **测试覆盖不均**：`TcpConnection` 只有 2 个用例（sendMulti、sendfile 交错）；
  `TcpServer` / `TcpClient` / `Connector` 还没有单元测试。
- **端口被占用时直接 abort**：`Acceptor` 里是 `LOG_ERROR` + `abort`（exit 134），
  不是友好提示；示例层没有拦截。
- **没有优雅退出**：未实现 `SignalHandler`（signalfd），进程收到 SIGINT/SIGTERM 直接退出。
- **还没有正式压测报告**：`benchmarks/bench_client` 已可用，但 QPS/延迟数据尚未整理成文档。
- **仅支持 IPv4**：`InetAddress` 只封装 `sockaddr_in`。
- **无连接级超时**：没有 idle timeout / 读写超时，慢连接会一直占用 fd。
- **sendfile 的降级路径吃内存**：输出缓冲非空时降级为 `pread` 整个文件再 append，
  大文件请求会占用与文件等量的用户态内存。
- **水位保护是每连接的，没有全局上限**：`echo_server_mt` 单条连接输出缓冲超过 8 MB
  会被断开，但 N 条慢连接最坏仍占用 N × 8 MB；手写版 `echo_server.cc` 完全没有保护。
- **日志是同步写 stdout**：没有异步双缓冲、文件滚动。
- **EventLoop 不可重启**：`quit()` 之后 `quit_` 保持为 1，不支持再次 `loop()`。
- **只支持 LT**：没有实现 ET 模式。
