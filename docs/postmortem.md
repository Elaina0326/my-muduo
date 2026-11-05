# my-muduo 事故复盘（postmortem）

> 只记录“出过问题、且值得留下教训”的事。编号是入库顺序，时间线看每条的提交号。
> 每条固定六段：现象 / 根因 / 为什么没被发现 / 修复 / 验证 / 教训。
> 时间线以 `git log` 为准，结论都能在本仓库复现。

| # | 一句话 | 引入 | 修复 |
|---|--------|------|------|
| 1 | `connectEstablished` 被调用两次，Debug 下每次建连都 abort | 0db157d（day27 IdleTimeout） | 2f9d65a（day28） |
| 2 | 慢消费者用例的“有界”断言是恒真式 | — | 2f9d65a |
| 3 | 水位通知只在慢路径被覆盖，快路径漏测 | — | 2f9d65a |
| 4 | 压测把客户端测成了瓶颈，180k 不是服务端上限 | — | 6997d24 之后 |
| 5 | 与参考实现“差 25–35%”，其实是示例里那行 LOG_INFO | — | — |
| 6 | CMakeLists 把 `tests/` 写成 `test/`，configure 直接失败 | — | — |
| 7 | `sendMulti` 超过 `IOV_MAX` 静默丢数据 | 移植期 | c5148f7 / 37fbd45（day16） |
| 8 | 线程池任务异常逃逸，进程无声消失 | 移植期 | 666a2d3（day17） |
| 9 | `EPOLLPRI` 注册了却没人消费，一字节打满一个核 | 6454dd8（day8） | f2d9b11（day21） |
| 10 | accept 把常态错误当致命，`EMFILE` 时忙循环 | 移植期 | 6118a66（day13）+ 414bcd6（day12） |
| 11 | `EventLoopThread` 析构时 `quit()` 往已关闭的 eventfd 上写 | day11 | c5148f7（day16） |
| 12 | `loop()` 开头清 `quit_`，退出意图丢失、`join()` 永久挂起 | 移植期 | c5148f7（day16） |
| 13 | `sendFile` 期间 `send()` 的数据被闷在 `outputBuffer_` | 移植期 | c5148f7（day16） |
| 14 | CMake 漏 TCP 源文件：构建全绿，1500 行代码没编译 | day14–15 | c5148f7（day16） |

---

## 1. Debug 下每次建连都 abort：`connectEstablished` 被调用两次

**现象**

Debug 构建下，任何 `TcpServer` 收到新连接都会让进程 abort：

```
unit_tests: muduo/net/TcpConnection.cc:712:
  void muduo::net::TcpConnection::connectEstablished():
  Assertion `state_ == kConnecting' failed.
```

`gdb` 调用栈从 `TcpServer::newConnection` 进入，第一次调用把 `state_` 从 `kConnecting` 推到 `kConnected`，第二次再进来断言就炸了。

**根因**

`TcpServer::newConnection` 里“建立连接”被写了两遍。day27 加 IdleTimeout 时新增了一个 lambda（建连 + 可选 `enableIdleTimeout`），但旧的那行没有删：

```cpp
ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn)); // 旧的，忘了删
const int idleSec = idleTimeoutSeconds_;
ioLoop->runInLoop([conn, idleSec] {
    conn->connectEstablished();
    if (idleSec > 0) conn->enableIdleTimeout(idleSec);
});
```

**为什么没被发现**

- 之前没有 TcpServer 级别的用例，`echo_server_mt` 是手工连的；
- 手工验证基本都用 Release 构建，`NDEBUG` 下 `assert` 被编译掉，表现不是崩溃，而是 `connectionCallback` 被调两次、`enableReading` 跑两次——静默重复，很难靠肉眼发现；
- 新增功能时没有回头检查“这条路径上是不是已经有一个等价调用”。

**修复**

删掉旧的那行，只保留带 `idleSec` 的 lambda：建立连接和开启空闲超时在同一次 `runInLoop` 里原子完成。

**验证**

- 新增 `TcpServerTest.ConnectionCallbackFiresOnce`：连一次，等 2s，断言 `connectedCalls == 1`；
- 修复前该用例直接 abort，修复后四个 TcpServer 用例全过。

**教训**

- 状态机的“入口”只能有一个；迁移代码时先搜同名字符串，再删旧调用。
- Debug 和 Release 的差别不只是性能：`assert` 消失会把“崩溃”变成“静默错误”，所以关键路径必须有用例，而不是只靠手工跑。
- 端到端用例（起 server + 真连）比单测更早暴露这类问题。

---

## 2. 慢消费者用例的“有界”断言是恒真式

**现象**

给慢消费者写的用例叫 `SlowConsumerOutputBufferBounded`，断言是：

```cpp
constexpr size_t kTotal = 1024 * 1024;      // 一次 send 1MB
EXPECT_LT(peak, kHighWaterMark + kTotal);   // 4KB + 1MB
```

`peak` 取自 `outputBuffer_` 的大小，上限就是这一次 `send` 的 1MB；而 `4KB + 1MB` 永远大于 1MB。这条断言在任何实现下都成立，等于没测。

**根因**

“上界 = 水位 + 一次写的量”这个式子成立的前提是**分块写**。一次 `send` 1MB，单次 append 就是 1MB，上界自然退化成 1MB。

**为什么没被发现**

- 断言写出来是绿的，没人去问“它在什么情况下会红”；
- 用例名给了虚假的安全感：名字叫 Bounded，实际没有任何一条断言在验证上界。

**修复**

改成“生产者尊重水位”的分块写法：

```cpp
constexpr size_t kChunk = 4 * 1024;
loop.runEvery(0.001, [&] {
    while (sent < kTotal && conn->outputBufferSize() < kHighWaterMark) {
        conn->send(std::string(kChunk, 'x'));
        sent += kChunk;
        peak = std::max(peak, conn->outputBufferSize());
    }
});
EXPECT_LT(peak, kHighWaterMark + kChunk);   // 真正有约束的上界
```

**验证**

变异：把“未到水位才追加”这个条件去掉，`peak = 1,040,448`，断言失败；恢复后通过。说明这条断言有反例。

**教训**

- 写断言前先问一句：**什么情况下它会红？** 答不上来就是恒真式。
- 顺带澄清一个边界：库本身不强制输出缓冲上限（`send` 多少就 append 多少），“有界”是应用侧契约——生产者在 `outputBufferSize() >= 水位` 时停，等 `writeComplete` 再继续。用例测的是这个契约，不是库的硬上限。
- 压测/测试里，“一次写多大”直接决定上界；大块写会掩盖背压问题。

---

## 3. 水位通知只在慢路径被覆盖，快路径漏测

**现象**

把 `sendMulti` 快路径 EAGAIN 分支里的 `notifyHighWaterMark(added)` 注释掉，原有的水位用例仍然通过。

**根因**

`notifyHighWaterMark` 有三个调用点：

| 行 | 路径 |
|----|------|
| 250 | `sendInLoop`：普通 send 追加后 |
| 299 | `sendMultiInLoop`：慢路径（已积压时追加） |
| 381 | `sendMultiInLoop`：快路径（直接 write 部分成功、剩余转 EAGAIN 追加） |

原用例用“预填内核缓冲区”的方式逼的是 299，381 从来没被走过。三处任一漏掉，都能让“跨水位通知”在某些流量形态下失效。

**修复**

补 `HighWaterMarkFiresOnceOnFastPath`：空连接直接 `sendMulti(8KB × 2)`，内核只收得下一部分，剩余走快路径追加并跨过水位；再追一批，断言回调只触发一次、参数 ≥ 水位。

**验证**

变异矩阵：只关 299 → 慢路径用例红；只关 381 → 快路径用例红；两处都开 → 两个用例都绿。

**教训**

- 同一个语义（“缓冲跨过水位”）在多个分支实现时，必须**逐条路径**覆盖，不能只测最容易构造的那条；
- 通知类逻辑还要测“只触发一次”——重复通知会让上层重复做决策。

---

## 4. 压测把客户端测成了瓶颈：180k 不是服务端上限

**现象**

报告里 `iothreads=8` 只有 180,346 QPS，看上去扩展性在 8 线程就停了；对照组里 `iothreads=4` 是 165,338，差距很小，一度判断“再加线程没用”。

**根因**

客户端只有 4 个线程、开着 100 条连接，每连接 batch 100 条——客户端先饱和了。换成 8 线程/800 连接重测，同一台机器、同一个二进制：

| 客户端配置 | io=8 QPS |
|-----------|----------|
| `-t 4 --connections 100`（原报告） | 180,346 |
| `-t 8 --connections 800`（复测 3 次） | 305,736 / 310,974 / 336,357 |
| `-t 16 --connections 1600` | 369,700 / 374,200 / 389,700（中位 374k，此时瓶颈又回到客户端） |

**为什么没被发现**

- 压测脚本只固定了服务端参数，没有“先证明客户端不是瓶颈”的步骤；
- 没有记录客户端 CPU，也没有做“加客户端线程 QPS 是否上涨”的对照。

**修复 / 做法**

- 结论改成“服务端 8 IO 线程 32–34 万 QPS（客户端 8 线程/800 连接）”，并注明“16 线程客户端时 37 万，瓶颈在客户端”；
- 报告里加一列“每请求服务端 CPU（µs）”，用 CPU 而不是 QPS 判断扩展性；
- 固定客户端配置，只改一个自变量。

**教训**

- 压测第一步永远是**证明客户端不是瓶颈**：加客户端线程 QPS 还涨，说明之前的数字是客户端上限。
- 数字必须带配置：线程数、连接数、batch、消息大小、时长，缺一个都不可比。
- `batch` 模式下的 P50/P99 是“一批的 RTT”，不是单条延迟，不能直接写进简历或报告。

---

## 5. 与参考实现“差 25–35%”，其实是示例里那行 LOG_INFO

**现象**

同机、同客户端、同参数对比，参考实现的 echo 示例 `io=8` 只有 245,899–251,979，而 my-muduo 的 `echo_server_mt` 是 305,736–336,357，看起来“快 25–35%”。

**根因**

参考示例的消息回调里每条消息都打一行 `LOG_INFO`，同步写日志把服务端拖慢了。把那一行去掉、用当前源码重编：

| 版本 | io=4 | io=8 |
|------|------|------|
| 参考示例原样（每条 LOG_INFO） | 153,184 / 156,152 / 156,789 | 245,899 / 246,041 / 251,979 |
| 参考示例去掉日志（源码重编） | 200,053 / 204,222 / 204,833 | 312,304 / 313,381 / 332,337 |
| my-muduo `echo_server_mt` | 198,416 / 201,582 / 212,337 | 305,736 / 310,974 / 336,357 |

去掉日志后两者持平（io=8 中位 313k vs 311k，差 <1%）。

**为什么差点误判**

- 参考仓库的 `build-rel` 是 10-18 编的、源码是 10-22 的，产物比源码旧；
- 对比时没先读对方的示例代码，默认“示例就是最小实现”。

**教训**

- 拿别人当基线前，先确认三件事：**产物和源码同版本、示例没开调试输出、参数一致**。
- “比 X 快 N 倍”这种结论，先做一次“把 X 的明显开销去掉”的对照，否则比的是对方的调试代码。
- 持平本身就是好结论：说明实现没有明显退化，卖点应该在背压、生命周期、测试和压测方法上，而不是绝对 QPS。

---

## 6. CMakeLists 把 `tests/` 写成 `test/`，configure 直接失败

**现象**

干净目录里 `cmake -S . -B build` 报：

```
Cannot find source file:
  test/test_tcpconnection.cc
```

**根因**

`unit_tests` 的源文件列表里路径拼成了 `test/`（少一个 s）。旧 build 目录是拼错之前配置的，所以本地一直“能编”。

**修复**

改成 `tests/`，并在干净目录重新 configure。

**教训**

- 本地旧 build 目录会掩盖配置错误；CI 或发布前必须**从零 configure**；
- 一条 `cmake -S . -B /tmp/x && cmake --build /tmp/x` 就能拦住这类问题，应该进 CI。

---

## 7. `sendMulti` 超过 `IOV_MAX`：静默丢数据

**现象**

day15 的临时 harness 里，`sendMulti` 传 2000 个分片（`IOV_MAX` 是 1024）时
`::writev` 直接返回 -1 / `EINVAL`，**客户端收到 0 字节**。
旧实现没有区分「调用失败」和「写完了」：剩余数据既没进 `outputBuffer_`，
也没有注册 `EPOLLOUT`，**而且没有任何错误日志**。

**根因**

`::writev` 的 `iovcnt` 上限是 `IOV_MAX`（Linux 上 1024）。超过上限不是部分写，
而是直接失败：返回 -1、`errno = EINVAL`。旧实现把「调用失败」当成「写完了」处理，
剩余数据既没有进 `outputBuffer_`，也没有注册 `EPOLLOUT`；更糟的一种情况是空缓冲时
仍然注册了写事件，`epoll_wait` 反复返回 `EPOLLOUT` 但每次都没数据可写。

**为什么没被发现**

- 正常业务一次 `sendMulti` 的分片数是个位数到几十，离 1024 很远；
- 早先的单测只发 1–2 个分片，全部走的是「一次 writev 成功」的路径；
- 失败是静默的，没有日志、没有崩溃，只有对端少收数据——不写对拍断言就看不出来。

**修复**

`TcpConnection.cc:283` 的 `sendMultiInLoop`：

1. 按 `IOV_MAX` 分批调用 `writev`，不再把全部 `iov` 一次塞进去；
2. 记录 `index`（处理到第几个分片）与剩余长度，`EAGAIN` 时把剩余追加到
   `outputBuffer_` 并 `enableWriting()`；
3. 部分写之后调整 `iov_base` / `iov_len` 续传，而不是从头重发。

**验证**

`tests/test_tcpconnection.cc:66` 的 `SendMultiOverIovMax`：2000 个分片，
断言对端收到的字节数与发送数**逐字节相同**。

**教训**

- 系统调用的上限要么查手册确认，要么写用例撞一次；「能跑通」只说明输入合法，不说明边界正确；
- 返回 -1 的路径必须和「返回 0」「返回部分」分开处理，三者的语义完全不同。

---

## 8. 线程池里的异常：进程无声消失

**现象**

投递一个必定抛异常的任务，进程立刻终止（shell 显示 `Aborted`）：
没有 crash 栈，没有 `LOG_ERROR`，日志最后一条是「任务开始」。

**根因**

异常从任务里抛出，穿过 `runInThread`，再穿过 pthread 的入口函数，
最终触发 `std::terminate()`。在服务端这表现为**进程定时消失**，
看起来像被 OOM killer 杀了，实际是未捕获异常。

**为什么没被发现**

- 业务任务从来不抛异常，正常路径永远走不到；
- 没有「故意让任务抛」的用例——测试都在验证功能，没人验证失败模式；
- 一旦发生就是进程级故障，现场什么都不剩，很难反推。

**修复**

`ThreadPool.cc:102` 起，两层隔离：

- 线程初始化回调（`threadInitCallback_`）单独 `try/catch`，失败只记日志，线程**继续进入任务循环**——池子的 worker 数量不能因为一次初始化失败而静默减少；
- 任务执行处 `catch (const std::exception&)` 与 `catch (...)`，记 `LOG_ERROR` 后继续取下一个任务。

关键点是**不让 worker 线程死掉**：一个任务失败不应该降低线程池的并发度。

**验证**

`tests/test_threadpool.cc:291` 的 `TaskThrowingDoesNotKillProcess`：
先投一个必抛异常的任务，再投一个正常任务，断言进程存活且后者确实执行了。

**教训**

- 线程池的失败模式是「进程消失」，不是「任务失败」——任何线程入口都必须有 `catch (...)`；
- 只测成功路径的测试，等于没测错误处理。

---

## 9. `EPOLLPRI`：一个字节打满一个核

**现象**

对照上游 `Channel::kReadEvent` 时发现它注册了 `EPOLLIN | EPOLLPRI | EPOLLRDHUP`，
而本项目没有任何读取 OOB（带外数据）的代码：全库 grep 不到 `MSG_OOB` / `SO_OOBINLINE`，
`handleRead` 走 `Buffer::readFd`（`readv`），**读不到**紧急字节。

**根因**

`EPOLLPRI` 对应 TCP 紧急数据。epoll 是**水平触发**的：只要这个字节没被读走，
每一次 `epoll_wait` 都会把它报成就绪。而本库没有 `MSG_OOB` 的 `recv`、也没有
`SO_OOBINLINE`，`handleRead` 走 `Buffer::readFd`（`readv`）**读不到**紧急字节。

注册一个永远不会被消费的事件，收益是 0，代价是一个字节就能拖垮一个核。

**为什么没被发现**

- 正常压测不会发 OOB，`MSG_OOB` 只在安全测试里出现；
- `kReadEvent` 是照着上游抄的，抄的时候没有问一句「谁来消费这个事件」；
- LT 模式下它不会报错，只会让 CPU 悄悄升高——`top` 里看到的是 `sys` 高，很容易归因到「系统忙」。

**修复**

`Channel.cc:16`：`kReadEvent = EPOLLIN`（`f2d9b11`）。对端关闭仍通过 `read()` 返回 0 识别，
`EPOLLRDHUP` 只是让关闭更早被感知，去掉后语义不变。

**验证**

**没有做 OOB 实验，也没有自动化用例。** 结论来自代码分析：注册了一个事件位却找不到
消费方，配合 LT 的语义（只要紧急字节未读走就持续就绪），必然是空转。
真要验证需要一个 `MSG_OOB` 客户端，成本高于收益——但这属于「**未经实验验证的推断**」，
引用时要说明这一点。

**教训**

- 抄架构可以，抄常量不行：每一个事件位都要能指出消费方；
- **别用「上游也这样」当理由**——上游的 `POLLIN | POLLPRI` 是 poll/epoll 共用常量的副作用，
  它同样没有消费代码、同样有忙等面。照抄一个缺陷不是理由。

---

## 10. accept 把常态错误当致命：日志洪水与 `EMFILE` 忙循环

**现象**

对比上游 `SocketsOps::accept` 时发现：它对每一次 `accept4` 失败都记 `LOG_SYSERR`，
未知错误直接 `LOG_FATAL`。而在 LT 模式下，`accept` 返回 `EAGAIN` 是**常态**——
一次事件里没 accept 完的连接还留在内核队列，下一轮 epoll 会再报一次。

**根因**

把「可恢复的常态」和「真正的错误」混在一个分支里，后果有两个：

1. 高并发建连时日志刷屏，同步写 stdout 会把 IO 线程拖住；
2. `EMFILE`（fd 耗尽）没有兜底——listen fd 始终可读，`epoll_wait` 立刻返回，
   空转打满一个核，日志里只有一行行 `accept: Too many open files`。

**为什么没被发现**

- 平时 fd 够用，撞不到 `EMFILE`；
- 日志刷屏只在很高并发下明显，而压测时盯的是 QPS，不看日志量；
- 单测里每个用例只建几条连接，`EAGAIN` 根本没出现过。

**修复**

`SocketsOps.cc:56` 按 errno 分两组：

- **可恢复**：`EAGAIN / ECONNABORTED / EINTR / EPROTO / EPERM / EMFILE` → 静默返回，交给调用方；
- **致命**：`EBADF / EFAULT / EINVAL / ENFILE / ENOBUFS / ENOMEM / ENOTSOCK` → `LOG_SYSERR` + `LOG_FATAL`。

`EMFILE` 单独在 `Acceptor.cc:60` 处理：预留一个 `idleFd_`（指向 `/dev/null`），
耗尽时先 `close(idleFd_)` 腾名额，`accept` 后立刻 `close`（对端收到 RST），
再把 `idleFd_` 重新打开。

**验证**

`tests/test_acceptor.cc` 4 条用例覆盖 accept 循环与「没有回调也要继续 accept」；
`EMFILE` 分支需要把 `ulimit -n` 压小再手工建连验证，暂无自动化用例。

**教训**

- 错误按「能不能继续」分类，而不是按「是不是错误」分类；
- 任何「fd 持续就绪 + 处理持续失败」的组合都会变成忙循环，必须有一个出口；
- `EPERM` 归进可恢复组：它一般来自防火墙拦截，对单个连接是失败，对进程不是。

---

## 11. `EventLoopThread` 析构时的 eventfd 竞态（2026-10-22）

**现象**

在参考实现上跑 TSan，偶发报出一条数据竞争：

```
Write of size 8 by T8:
  #0 close
  #1 muduo::net::EventLoop::~EventLoop()        EventLoop.cc
  #2 muduo::net::EventLoopThread::threadFunc()  EventLoopThread.cc
Previous read of size 8 by T3:
  #0 write
  #1 muduo::net::EventLoop::wakeup()            EventLoop.cc
  #2 muduo::net::EventLoop::quit()
  #3 muduo::net::EventLoopThread::~EventLoopThread()
```

同一个 eventfd：loop 线程在 `~EventLoop` 里 `close`，主线程在 `quit()` 里 `write`。
轻则 TSan 报 UB，重则写进一个已经被复用的 fd。

**根因**

`~EventLoopThread` **无锁**读 `loop_`，再调用 `quit()`。而 loop 线程可能在这两步之间跑完
`loop()`、把 `loop_` 置空、析构 `EventLoop` 并关闭 `wakeupFd_`：

```
main:        读到 loop_（非空）
loop 线程:   loop() 返回 → loop_ = nullptr → ~EventLoop → close(wakeupFd_)
main:        quit() → wakeup() → write(已关闭的 fd)
```

**为什么没被发现**

- 窗口只有几条指令，普通用例的“启动 → 停 → 析构”几乎撞不上；
- 没有 TSan 时表现为偶发崩溃或写坏别人的 fd，不是稳定失败；
- 析构里读指针太自然了，没人会先问“这个对象还活着吗”。

**修复**

`c5148f7`：用 `mutex_` 把“读 `loop_` + `quit()`”和“置空 `loop_` + 析构 `EventLoop`”串行化。

- `~EventLoopThread` 持锁读取 `loop_` 并调用 `quit()`，释放锁后再 `join()`；
- `threadFunc` 在同一把锁下把 `loop_` 置空；
- `join()` 保证析构真正返回时，loop 线程已经结束。

**验证**

参考实现修复后 TSan 全量用例 0 告警；my-muduo 同步这份代码后用 TSan 复跑全部用例，
0 告警。

**教训**

- 跨线程共享的裸指针，**读出来和使用必须处于同一个临界区**；先读快照再假设对象还活着，等于留了一个 UAF/竞态窗口；
- 生命周期问题优先用“锁 + join”建立明确的 happens-before，而不是靠“应该不会这么快”。

---

## 12. `loop()` 把 `quit_` 清零：退出意图丢失、`join()` 永久挂起（2026-10-22）

**现象**

给 `EventLoopThread` 加压力用例（启动后立刻析构 / 启动后立刻 `quit()`，各 1000 轮）。
把 `loop()` 开头的 `quit_.set(0)` 加回去后，`--gtest_repeat=20` 在 60 秒内只跑到第 10 轮
就卡死；删掉这一行后全部通过。

**根因**

```
threadFunc: loop_ = &loop;              // 已发布
main:       拿锁 → quit() → quit_ = 1
threadFunc: loop() → quit_.set(0);      // 退出意图被清掉
            while (quit_ == 0) ...      // 永远不退出 → ~EventLoopThread 的 join() 挂起
```

`wakeup()` 只能让 `epoll_wait` 立刻返回一次，它不会把 `quit_` 重新置 1。
所以“叫醒”不等于“收到退出”。

**为什么没被发现**

- 窗口在“发布 `loop_` 之后、进入 `loop()` 之前”，纳秒级；
- 普通功能测试不会在启动瞬间析构线程；
- 上游 muduo 也这么写，照抄时没有质疑。

**修复**

`c5148f7`：删掉 `loop()` 里的 `quit_.set(0)`。`quit_` 构造时就是 0，`quit()` 只置 1；
启动前被 quit 的 loop 会立刻返回。

代价是 EventLoop 不再支持“退出后重新 loop”，本来也没有这个语义。

**验证**

- 反向验证（用临时压力程序，尚未入库）：程序做“启动后立刻析构 / 启动后立刻 `quit()`”
  各 1000 轮；把 `quit_.set(0)` 加回去后 `--gtest_repeat=20` 在 60 秒内卡在第 10 轮，
  删掉后 1000 轮 × 20 次全部通过。

**教训**

- 退出标志只能**置位**，不能在消费者入口清零；
- 压力测试要针对“状态发布与状态消费之间的窗口”，而不是只测正常顺序；
- “上游也这样”不是理由。

---

## 13. `sendFile` 期间的 `send()`：数据被闷在 `outputBuffer_`（2026-10-22）

**现象**

先 `sendFile` 一个 8MB 文件，紧接着 `send("TAIL")`（4 字节）。客户端只收到
**8,388,608 / 8,388,612** 字节，`TAIL` 永远不到；`writeCompleteCallback_` 还提前触发了一次。

**根因**

- `handleWrite()` 看到 `sendFileActive_` 就 `return`，完全不处理 `outputBuffer_`；
- `continueSendFile()` 发完文件后无条件 `disableWriting()` + 触发写完成回调，
  不检查缓冲里还有没有 `send()` 排队的数据。

于是文件发送期间追加的普通数据，既没有被写出去，也没有保留 EPOLLOUT 等下一次机会。

**为什么没被发现**

- 典型静态文件响应是“响应头 + 文件体”，不会在 sendfile 期间再 `send`；
- 单测只校验文件本身，没有在挂起期间追加数据；
- `writeComplete` 提前触发也不会崩，只是调用方误以为响应发完了。

**修复**

`c5148f7`：

- `continueSendFile()` 完成后先看 `outputBuffer_`：非空就保持 EPOLLOUT 并返回，
  不 `disableWriting`、不触发写完成；
- `handleWrite()` 在 sendfile 结束后不再直接 return，而是落到普通缓冲分支继续冲刷；
- 真正把缓冲发空后才 `disableWriting()` + `writeCompleteCallback_`。

**验证**

`tests/test_tcpconnection.cc` 的 `SendFileThenSendFlushesBufferedData`：
256KB 文件 + 4 字节 TAIL，修复前测试收不到 TAIL（读超时），修复后逐字节一致。

**教训**

- “写完成”必须定义为**所有排队数据都已写出**，而不是“当前正在用的那条路径写完了”；
- 两条发送路径（sendfile / outputBuffer）共存时，要明确谁先谁后、切换点在哪里；
- 回调的触发时机是接口契约的一部分，提前触发比不触发更难排查。

---

## 14. CMake 漏了 TCP 源文件：构建全绿，但 1500 行代码没编译（2026-10-22）

**现象**

day14/day15 提交了 `TcpConnection` / `TcpServer` / `TcpClient` / `Connector` / `Callbacks`
约 1500 行。`cmake --build` 全绿，`unit_tests` 127/127，但可执行文件里根本找不到这些符号。

**根因**

`CMakeLists.txt` 的 `MUDUO_SRCS` 是**显式列表**，新增的 `.cc` 没有加进去。
构建只编译列出的源文件；没有代码引用它们，所以也不会产生链接错误。

**为什么没被发现**

- “构建成功 + 测试全绿”给人强烈错觉；
- 新文件没被任何测试或示例引用，链接器没有任何理由报错；
- 本地旧 build 目录也不会提示“有新文件没编译”。

**修复**

- `c5148f7` 把 5 个 `.cc` 加进 `MUDUO_SRCS`；
- `37fbd45` 修正 `tests/` 路径拼写（少一个 `s`，干净 configure 才会暴露）。

**验证**

编译日志里出现 `TcpConnection.cc` / `TcpServer.cc` 等文件的编译行；随后新增的
`test_tcpconnection.cc` 才真正链接到这些实现。

**教训**

- **“构建成功”只证明已列出的代码能编译**，不证明你新写的代码参与了构建；
- 新增源文件后，要么看编译日志，要么从零 configure 一次；
- 用显式源文件列表时，把“加文件”当成一条 checklist，而不是事后补。

---

## 附：这套复盘怎么用

面试里讲项目，最容易被追问的是“你遇到过什么问题、怎么定位的”。这份文档就是素材库，挑一条讲清楚四步即可：

1. **现象**：能复现的报错/数字；
2. **定位**：怎么缩到那一行（gdb、二分、变异、对比）；
3. **根因**：一句话说清机制；
4. **验证**：改完用什么证明好了（用例 + 变异）。

比背“reactor 模型有几种”有说服力得多。
