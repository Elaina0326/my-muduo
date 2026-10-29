# bench_client.cc 审查（v7）

> **对象**：`benchmarks/bench_client.cc`（746 行，2026-10-28 09:02）
> **方式**：只读通读，**未编译、未运行**
> **结论**：代码层面已收敛。剩 1 个未限流的小口子 + 2 个"接受现状、写进文档"的决定。

---

## 一、你提的三条：两成立，一条要更正

### 1.1 `sampleCap` 是前缀采样 —— 成立，接受

```cpp
if (out->latenciesUs.size() < sampleCap) {
    out->latenciesUs.push_back(nowUs() - begin);
} else {
    out->sampleCapped = true;      // 只是标记，采样本身就是"前 N 轮"
}
```

要真修，得改成每 k 轮采一次（`if (round % k == 0)`），k 由 `预计轮数 / sampleCap` 估出来。但要估准预计轮数，又得先知道 QPS —— 绕回去了。

**当前规模下不值得改**：几十秒、≤8 线程、sampleCap = 4M/8 = 50 万，`capped=false` 基本不会触发。写进 README 说明就够。

---

### 1.2 "连接失败仍逐条打印" —— **部分过期，位置不对**

`connect` 那一层**已经限流了**，7 条失败路径全走 `logConnectFailure`：

| 行 | 内容 |
|---|---|
| L127 | invalid IPv4 address |
| L138 / L167 | connect failed |
| L153 | connect timeout |
| L158 | poll failed |
| L180 | fcntl(O_NONBLOCK clear) failed |
| L189 | setsockopt(TCP_NODELAY) failed |

**真正没限流的是校验与预热阶段这三处**（L314 / L322 / L347）：

```cpp
std::fprintf(stderr, "thread %d: validation failed: %s\n", id, ...);   // L314
std::fprintf(stderr, "thread %d: echo content mismatch (%zu bytes)\n", ...);  // L322
std::fprintf(stderr, "thread %d: warmup failed: %s\n", id, ...);       // L347
```

而且这三处**比 connect 更容易刷屏**——connect 失败说明连不上，通常整批一起失败（限流后只打 5 条就够）；而校验失败是**每条连接各自独立的事件**，`--connections 1000` 服务端行为异常时就是 1000 行。

**修法**（约 10 行）：把这三处也改走 `logConnectFailure`，或另起一个 `logRuntimeFailure`（同样的 `atomic` 计数，但文案区分开）。

```cpp
void logRuntimeFailure(const char* fmt, ...) {   // 与前缀一致：前 5 条 + 一行 suppressed
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1) >= 6) return;
    /* ... vsnprintf + fprintf ... */
}
```

> 建议 `connect` 与 `runtime` 各用一套计数器——两类错误的排查方向完全不同，混在一起会漏掉其中一类。

---

### 1.3 无 CSV —— 属实，保持可选

现在靠肉眼从 stdout 抄数字进 README。跑四组就抄 16 个数，容易出错。但这不是阻塞项，等真要多跑几轮对比时再加。

---

## 二、README 压测小节（可直接粘）

> 下面这段建议插在 `README.md` 的「测试」之后。数字留空，等你跑完填。

```markdown
## 压测

环境：WSL2 / loopback（127.0.0.1）/ g++ <版本> -O2 / <CPU 型号> / <核数>
服务端与客户端同机，且服务端为单进程多线程，**数字只用于本机前后对比，不作跨机比较**。

| 场景 | 命令 | QPS | P50 | P99 | 完成数 | 失败 |
|---|---|---|---|---|---|---|
| 单连接基线 | `bench_client 127.0.0.1 8888 1 20000 1024` | | | | | |
| 4 IO 线程 | `bench_client 127.0.0.1 8888 4 20000 1024` | | | | | |
| 长稳 60s | `bench_client 127.0.0.1 8888 8 0 64 --duration 60` | | | | | |
| 1000 连接 | `bench_client 127.0.0.1 8888 8 2000 64 --connections 1000` | | | | | |

**口径说明**

- 延迟只在 `connections <= threads`（输出标 `1 msg/round`）时可横向比较；
  `batch RTT` 是一轮内多条消息的总耗时，随连接数变化，不能跨配置比。
- `capped` 表示样本数触及全局配额，**分位数只覆盖前段样本（前缀采样），不代表全程分布**。
- QPS 的分母是门闩窗口（所有线程统一发令 → 最后一个线程结束），不是各线程 activeUs 之和。
```

### 跑之前确认三件事

| 检查 | 怎么做 |
|---|---|
| 客户端不是瓶颈 | 4 线程 QPS 若 ≈ 1 线程，先把客户端线程数加到 8 再测 |
| 服务端没在 Debug | 确认用的是 `build-rel`，`-O2 -DNDEBUG` |
| 端口没被占 | `ss -lntp \| grep 8888`，避免连到上次残留的进程 |

---

## 三、代码层面还剩什么

| 项 | 状态 | 建议 |
|---|---|---|
| 校验/预热的 stderr 未限流 | ❌ 未改 | 10 行，建议改（比 connect 更容易刷屏） |
| `sampleCap` 前缀采样 | ⚠️ 已标记未修 | 接受，写进 README |
| 无 CSV | ⚠️ 未做 | 可选，等要横向对比多轮时再说 |
| `max` 没带口径标签 | ⚠️ 未改 | 1 行，顺手 |
| pipelining 模式 | ⚠️ 未做 | 12 月可选，别和并发模式混着讲 |

---

## 四、还没验证的部分

**这份审查没编译过。** `wsl.exe` 被安全策略拦死，只能读文件。

第一次跑 `--duration 60` 时，盯住 `elapsed` 是不是 59.5–60.5。不是的话，先确认 `messages` 传的是 0（虽然现在会被忽略，但传错位置会吃掉后面的 `--duration`）。

---

*本文只读审查产出，未修改任何源码。行号以 2026-10-28 09:02 的版本为准。*
