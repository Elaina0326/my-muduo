// bench_client.cc —— echo 服务器压测客户端
//
// 用法：
//   ./bench_client <host> <port> <threads> <messages_per_conn>
//                  [payload_bytes] [--connections N]
//
// 示例：
//   ./bench_client 127.0.0.1 8888 8 20000 64
//   ./bench_client 127.0.0.1 8888 8 2000 64 --connections 1000
//
// 参数：
//   threads          客户端线程数
//   messages_per_conn 每条连接发送的请求数（每轮每个连接发一条）
//   payload_bytes    单条消息字节数，1..1048576，默认 64
//   --connections N  总连接数，默认等于 threads（每线程一条）
//
// 设计要点：
//   ① 每条连接建立后设置 TCP_NODELAY。不设的话 Nagle 会把小包攒到
//      40ms 再发，P99 直接被污染。
//   ② 先 warmup 1 秒（不计入统计），所有线程 warmup 完再统一计时。
//   ③ ping-pong：发一条 → 等回显 → 再发下一条，测的是 RTT。
//      多连接时，每轮向所有连接各发一条，再依次收齐。
//   ④ 多线程提交，避免压测机单核成为瓶颈。
//   ⑤ 统计 QPS、P50/P99、连接建立成功率，并区分超时与 IO 错误。
//
// 压测请用 Release 构建：
//   cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-rel -j4

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWarmupSeconds = 1;
constexpr int kConnectTimeoutMs = 5000;   // 单次非阻塞 connect 的 poll 超时
constexpr int kConnectBudgetSec = 30;     // 建连阶段总预算（防止 1000×5s）
constexpr int kIOTimeoutSec = 5;          // send/recv 的 SO_SNDTIMEO/SO_RCVTIMEO
constexpr size_t kMaxPayload = 1u << 20;  // 1 MiB
constexpr int kMaxThreads = 4096;
constexpr long long kMaxMessages = 1000000;
constexpr size_t kMaxTotalSamples = 1u << 22;  // 全进程样本上限（约 32MB）

// 一次 send/recv 的结果。超时和真错误要分开统计：
//   · 超时说明服务端慢（可以调优）；
//   · 连接被重置/对端关闭说明服务端或网络出了问题。
enum class IoResult {
    Ok,
    Timeout,   // EAGAIN / EWOULDBLOCK（SO_SNDTIMEO/SO_RCVTIMEO 触发）
    Error      // 其它错误，或对端关闭（send/recv 返回 0）
};

uint64_t nowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 连接失败信息限流：1000 条连接全失败时只打前 5 条，避免刷屏。
void logConnectFailure(const char* fmt, ...) {
    static std::atomic<int> printed{0};
    const int n = printed.fetch_add(1);
    if (n >= 6) {
        return;
    }

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 5) {
        std::fprintf(stderr, "%s\n", buf);
    } else {
        std::fprintf(stderr, "further connect failures suppressed\n");
    }
}

// 非阻塞 connect + poll 超时。
//
// 为什么要这么写：SO_SNDTIMEO/SO_RCVTIMEO 只作用于 send/recv，
// 不影响 connect。对端不回 SYN 时，阻塞 connect 会卡到内核重试超时
// （Linux 约 2 分钟），压测脚本会像死机一样。
int connectTo(const char* host, uint16_t port, int timeoutMs) {
    static std::atomic<bool> emfileWarned{false};

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if ((errno == EMFILE || errno == ENFILE) &&
            !emfileWarned.exchange(true)) {
            std::fprintf(stderr,
                         "socket() failed: %s — 连接数太多，先提高 ulimit -n"
                         "（例如 ulimit -n 100000）\n",
                         std::strerror(errno));
        }
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        logConnectFailure("invalid IPv4 address: %s "
                          "(只支持 IPv4 字面量，不支持 localhost/域名)",
                          host);
        ::close(fd);
        return -1;
    }

    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr));
    if (ret != 0) {
        if (errno != EINPROGRESS) {
            logConnectFailure("connect failed: %s", std::strerror(errno));
            ::close(fd);
            return -1;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;

        int pollRet = 0;
        do {
            pollRet = ::poll(&pfd, 1, timeoutMs);
        } while (pollRet < 0 && errno == EINTR);

        if (pollRet == 0) {
            logConnectFailure("connect timeout (%d ms)", timeoutMs);
            ::close(fd);
            return -1;
        }
        if (pollRet < 0) {
            logConnectFailure("poll failed: %s", std::strerror(errno));
            ::close(fd);
            return -1;
        }

        int soError = 0;
        socklen_t len = sizeof(soError);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) != 0 ||
            soError != 0) {
            logConnectFailure("connect failed: %s",
                              std::strerror(soError != 0 ? soError : errno));
            ::close(fd);
            return -1;
        }
    }

    // 恢复阻塞模式：后面用 SO_RCVTIMEO/SO_SNDTIMEO 控制收发超时。
    // 失败必须关闭连接 —— 否则 fd 仍是非阻塞，之后每次 send/recv 都会
    // 立刻 EAGAIN，被统一误记成"服务端超时"。
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        logConnectFailure("fcntl(O_NONBLOCK clear) failed: %s",
                          std::strerror(errno));
        ::close(fd);
        return -1;
    }

    // ★ 必须关掉 Nagle：否则小包的 P99 会被 40ms 延迟主导
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        logConnectFailure("setsockopt(TCP_NODELAY) failed: %s",
                          std::strerror(errno));
    }

    struct timeval tv{};
    tv.tv_sec = kIOTimeoutSec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

IoResult sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IoResult::Timeout;
            }
            return IoResult::Error;
        }
        if (n == 0) {
            return IoResult::Error;
        }
        sent += static_cast<size_t>(n);
    }
    return IoResult::Ok;
}

IoResult recvExact(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IoResult::Timeout;
            }
            return IoResult::Error;
        }
        if (n == 0) {
            return IoResult::Error;   // 对端关闭
        }
        got += static_cast<size_t>(n);
    }
    return IoResult::Ok;
}

IoResult pingPong(int fd, const std::string& payload, std::string& reply) {
    IoResult r = sendAll(fd, payload.data(), payload.size());
    if (r != IoResult::Ok) {
        return r;
    }
    return recvExact(fd, &reply[0], reply.size());
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int ready = 0;
    int finished = 0;
    bool go = false;
};

struct ThreadResult {
    size_t attempted = 0;     // 本线程真正发起过的 connect 次数
    size_t connected = 0;     // 建连成功并通过校验 + warmup 的可用连接
    uint64_t completed = 0;   // 完成的请求数（每轮 × 连接数）
    uint64_t timeouts = 0;
    uint64_t ioErrors = 0;
    uint64_t failedRounds = 0;   // 有连接失败、因此未计入延迟样本的轮次
    uint64_t activeUs = 0;       // 计时阶段实际运行时长
    bool sampleCapped = false;   // 是否因全局样本配额而丢弃过样本
    std::vector<uint64_t> latenciesUs;   // 每轮的 RTT 样本
};

void recordIo(ThreadResult* out, IoResult r) {
    if (r == IoResult::Timeout) {
        ++out->timeouts;
    } else if (r == IoResult::Error) {
        ++out->ioErrors;
    }
}

// 单个压测线程：连接 → 校验回显 + warmup → 等发令 → 计时。
void worker(int id, const char* host, uint16_t port, size_t conns,
            size_t messages, size_t payloadSize, size_t sampleCap,
            double durationSec, Gate* gate, ThreadResult* out) {
    std::vector<int> fds;
    fds.reserve(conns);

    // 建连阶段有总预算：单次 connect 最多等 5 秒，如果对端完全不回 SYN，
    // 1000 条连接最坏会等 83 分钟。超过 30 秒就不再继续尝试。
    const auto connectDeadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(kConnectBudgetSec);

    for (size_t i = 0; i < conns; ++i) {
        if (std::chrono::steady_clock::now() > connectDeadline) {
            std::fprintf(stderr,
                         "thread %d: connect budget (%ds) exhausted at "
                         "%zu/%zu connections\n",
                         id, kConnectBudgetSec, i, conns);
            break;
        }
        ++out->attempted;   // 只有真正发起过的 connect 才计入分母
        int fd = connectTo(host, port, kConnectTimeoutMs);
        if (fd >= 0) {
            fds.push_back(fd);
        }
    }

    std::string payload(payloadSize, 'x');
    std::string reply(payloadSize, '\0');

    // 每条连接先跑一次，验证回显内容。失败的那条摘掉，不影响其它连接。
    for (size_t i = 0; i < fds.size();) {
        IoResult r = pingPong(fds[i], payload, reply);
        if (r != IoResult::Ok) {
            std::fprintf(stderr, "thread %d: validation failed: %s\n", id,
                         r == IoResult::Timeout ? "timeout" : "io error");
            recordIo(out, r);
            ::close(fds[i]);
            fds.erase(fds.begin() + static_cast<long>(i));
            continue;
        }
        if (reply != payload) {
            std::fprintf(stderr,
                         "thread %d: echo content mismatch (%zu bytes)\n",
                         id, payloadSize);
            ++out->ioErrors;   // 协议层错误，不能当成正常连接
            ::close(fds[i]);
            fds.erase(fds.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }

    // warmup 1 秒：轮转所有连接，不计入统计。
    // 单条连接失败只摘掉它，剩下的继续 warmup。
    {
        const auto warmupEnd =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(kWarmupSeconds);
        size_t next = 0;
        while (!fds.empty() &&
               std::chrono::steady_clock::now() < warmupEnd) {
            const size_t idx = next % fds.size();
            ++next;

            IoResult r = pingPong(fds[idx], payload, reply);
            if (r != IoResult::Ok) {
                std::fprintf(stderr, "thread %d: warmup failed: %s\n", id,
                             r == IoResult::Timeout ? "timeout" : "io error");
                recordIo(out, r);
                ::close(fds[idx]);
                fds.erase(fds.begin() + static_cast<long>(idx));
                next = 0;
            }
        }
    }

    // ★ 可用连接 = 建连成功 + 回显正确 + warmup 没挂。
    //   必须放在 warmup 之后，否则会出现"100% 成功 + 0 样本"的自相矛盾数字。
    out->connected = fds.size();

    // 报到并等待统一发令
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        ++gate->ready;
        gate->cv.notify_all();
        gate->cv.wait(lock, [gate] { return gate->go; });
    }

    // 计时阶段：每轮向所有连接各发一条，再依次收齐。
    // 单条连接失败只摘掉它，其余连接继续跑；全部失败才退出。
    if (!fds.empty()) {
        const uint64_t timedBegin = nowUs();
        const auto timedDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(
                static_cast<long long>(durationSec * 1000.0));

        // duration 模式只看时间，忽略 messages（避免"以为跑 60s、实际 0.1s"）；
        // 固定轮数模式才把 messages 当作循环上限。
        const size_t loopLimit =
            (durationSec > 0.0)
                ? std::numeric_limits<size_t>::max()
                : messages;
        out->latenciesUs.reserve(std::min<size_t>(loopLimit, sampleCap));

        for (size_t round = 0; round < loopLimit; ++round) {
            if (durationSec > 0.0 &&
                std::chrono::steady_clock::now() >= timedDeadline) {
                break;
            }

            const uint64_t begin = nowUs();
            size_t succeeded = 0;
            bool hadFailure = false;

            // send 阶段：失败的 fd 先标 -1，本轮结束后统一摘掉
            for (size_t i = 0; i < fds.size(); ++i) {
                if (fds[i] < 0) {
                    continue;
                }
                IoResult r = sendAll(fds[i], payload.data(), payload.size());
                if (r != IoResult::Ok) {
                    recordIo(out, r);
                    ::close(fds[i]);
                    fds[i] = -1;
                    hadFailure = true;
                } else {
                    ++succeeded;
                }
            }

            // recv 阶段
            for (size_t i = 0; i < fds.size(); ++i) {
                if (fds[i] < 0) {
                    continue;
                }
                IoResult r = recvExact(fds[i], &reply[0], reply.size());
                if (r != IoResult::Ok) {
                    recordIo(out, r);
                    ::close(fds[i]);
                    fds[i] = -1;
                    --succeeded;   // 这条消息没有完成
                    hadFailure = true;
                }
            }

            fds.erase(std::remove(fds.begin(), fds.end(), -1), fds.end());
            out->completed += succeeded;

            // 本轮没有失败才记一个延迟样本，避免错误路径的耗时污染分位数
            if (!hadFailure) {
                if (out->latenciesUs.size() < sampleCap) {
                    out->latenciesUs.push_back(nowUs() - begin);
                } else {
                    out->sampleCapped = true;
                }
            } else {
                ++out->failedRounds;
            }

            if (fds.empty()) {
                break;
            }
        }

        out->activeUs = nowUs() - timedBegin;

        for (int fd : fds) {
            ::close(fd);
        }
    }

    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        ++gate->finished;
        gate->cv.notify_all();
    }
}

// 最近秩百分位（nearest-rank）。
double percentile(const std::vector<uint64_t>& sortedUs, double p) {
    if (sortedUs.empty()) {
        return 0.0;
    }
    size_t rank = static_cast<size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sortedUs.size())));
    if (rank == 0) {
        rank = 1;
    }
    if (rank > sortedUs.size()) {
        rank = sortedUs.size();
    }
    return static_cast<double>(sortedUs[rank - 1]);
}

void usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s <host> <port> <threads> <messages_per_conn> "
        "[payload_bytes] [--connections N] [--duration S]\n"
        "  host          : IPv4 字面量（如 127.0.0.1），不支持 localhost/域名\n"
        "  messages_per_conn : 固定轮数模式下 >=1；--duration 模式下忽略，可传 0\n"
        "  payload_bytes : 1..1048576 (default 64)\n"
        "  --connections : total connections, default = threads\n"
        "  --duration    : run for S seconds; messages_per_conn 不再作为上限\n"
        "  exit codes    : 0=all good, 1=bad args, 2=partial failure, "
        "3=total failure\n"
        "  example: %s 127.0.0.1 8888 8 20000 64\n"
        "  example: %s 127.0.0.1 8888 4 0 64 --duration 60\n"
        "  example: %s 127.0.0.1 8888 8 2000 64 --connections 1000\n",
        prog, prog, prog, prog);
}

// 严格解析非负整数，失败返回 -1。
long long parseNonNegative(const char* s) {
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0) {
        return -1;
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }

    const char* host = argv[1];

    const long long portValue = parseNonNegative(argv[2]);
    if (portValue < 1 || portValue > 65535) {
        std::fprintf(stderr, "port 必须在 1..65535 之间\n");
        return 1;
    }

    const long long threadsValue = parseNonNegative(argv[3]);
    if (threadsValue < 1 || threadsValue > kMaxThreads) {
        std::fprintf(stderr, "threads 必须在 1..%d 之间\n", kMaxThreads);
        return 1;
    }

    const long long messagesValue = parseNonNegative(argv[4]);
    if (messagesValue < 0 || messagesValue > kMaxMessages) {
        std::fprintf(stderr,
                     "messages_per_conn 必须在 0..%lld 之间"
                     "（0 仅用于 --duration 模式）\n",
                     kMaxMessages);
        return 1;
    }

    size_t payloadSize = 64;
    int argi = 5;
    if (argi < argc && std::strncmp(argv[argi], "--", 2) != 0) {
        const long long payloadValue = parseNonNegative(argv[argi]);
        if (payloadValue < 1 ||
            static_cast<unsigned long long>(payloadValue) > kMaxPayload) {
            std::fprintf(stderr,
                         "payload_bytes 必须在 1..%zu 之间\n", kMaxPayload);
            return 1;
        }
        payloadSize = static_cast<size_t>(payloadValue);
        ++argi;
    }

    size_t connections = static_cast<size_t>(threadsValue);
    long long durationValue = 0;   // 0 = 固定 messages，>0 = 跑固定秒数
    for (; argi < argc; ++argi) {
        if (std::strcmp(argv[argi], "--connections") == 0 && argi + 1 < argc) {
            const long long v = parseNonNegative(argv[++argi]);
            if (v < 1 || v > 1000000) {
                std::fprintf(stderr,
                             "--connections 必须在 1..1000000 之间\n");
                return 1;
            }
            connections = static_cast<size_t>(v);
        } else if (std::strcmp(argv[argi], "--duration") == 0 &&
                   argi + 1 < argc) {
            const long long v = parseNonNegative(argv[++argi]);
            if (v < 1 || v > 3600) {
                std::fprintf(stderr, "--duration 必须在 1..3600 秒之间\n");
                return 1;
            }
            durationValue = v;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[argi]);
            usage(argv[0]);
            return 1;
        }
    }

    if (durationValue == 0 && messagesValue == 0) {
        std::fprintf(stderr,
                     "固定轮数模式必须给 messages_per_conn >= 1；"
                     "如果用 --duration，可以把它传 0\n");
        return 1;
    }

    const uint16_t port = static_cast<uint16_t>(portValue);
    const int threads = static_cast<int>(threadsValue);
    const size_t messages = static_cast<size_t>(messagesValue);
    const double durationSec = static_cast<double>(durationValue);

    // 全进程样本上限按线程均分，避免 threads=4096 时聚合内存爆掉
    const size_t sampleCap =
        std::max<size_t>(1, kMaxTotalSamples / static_cast<size_t>(threads));

    if (durationValue > 0) {
        std::printf("target=%s:%u connections=%zu threads=%d "
                    "duration=%llds (messages arg ignored) payload=%zuB "
                    "warmup=%ds\n",
                    host, port, connections, threads, durationValue,
                    payloadSize, kWarmupSeconds);
    } else {
        std::printf("target=%s:%u connections=%zu threads=%d "
                    "messages/conn=%zu payload=%zuB warmup=%ds\n",
                    host, port, connections, threads, messages, payloadSize,
                    kWarmupSeconds);
    }

    std::vector<ThreadResult> results(static_cast<size_t>(threads));
    Gate gate;

    const size_t baseConns = connections / static_cast<size_t>(threads);
    const size_t extraConns = connections % static_cast<size_t>(threads);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        const size_t conns =
            baseConns + (static_cast<size_t>(i) < extraConns ? 1 : 0);
        workers.emplace_back(worker, i, host, port, conns, messages,
                             payloadSize, sampleCap, durationSec, &gate,
                             &results[static_cast<size_t>(i)]);
    }

    uint64_t elapsedUs = 0;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.cv.wait(lock, [&gate, threads] { return gate.ready == threads; });

        const uint64_t begin = nowUs();
        gate.go = true;
        gate.cv.notify_all();

        gate.cv.wait(lock,
                     [&gate, threads] { return gate.finished == threads; });
        elapsedUs = nowUs() - begin;
    }

    for (std::thread& t : workers) {
        t.join();
    }

    // ---- 汇总 ----
    size_t attempted = 0;
    size_t connected = 0;
    uint64_t completed = 0;
    uint64_t timeouts = 0;
    uint64_t ioErrors = 0;
    uint64_t failedRounds = 0;
    uint64_t latencySumUs = 0;
    bool sampleCapped = false;
    std::vector<uint64_t> latencies;

    for (const ThreadResult& r : results) {
        attempted += r.attempted;
        connected += r.connected;
        completed += r.completed;
        timeouts += r.timeouts;
        ioErrors += r.ioErrors;
        failedRounds += r.failedRounds;
        sampleCapped = sampleCapped || r.sampleCapped;
        for (uint64_t us : r.latenciesUs) {
            latencySumUs += us;
        }
        latencies.insert(latencies.end(), r.latenciesUs.begin(),
                         r.latenciesUs.end());
    }

    std::sort(latencies.begin(), latencies.end());

    const double elapsedSec = static_cast<double>(elapsedUs) / 1e6;
    const double qps =
        elapsedSec > 0.0 ? static_cast<double>(completed) / elapsedSec : 0.0;

    std::printf("------------------------------------------\n");
    std::printf("connect success : %zu/%zu (%.1f%%)\n", connected, attempted,
                attempted > 0 ? 100.0 * static_cast<double>(connected) /
                                    static_cast<double>(attempted)
                              : 0.0);
    std::printf("completed       : %llu requests, timeout=%llu, "
                "io_error=%llu\n",
                static_cast<unsigned long long>(completed),
                static_cast<unsigned long long>(timeouts),
                static_cast<unsigned long long>(ioErrors));
    std::printf("elapsed         : %.3f s\n", elapsedSec);
    std::printf("QPS             : %.1f\n", qps);

    // 每线程最多一条连接时，一轮 = 一条消息，延迟就是单请求 RTT；
    // 连接数多于线程数时一轮包含多条消息，P50/P99 是"批量"口径。
    const bool batchMode = (connections > static_cast<size_t>(threads));
    const char* scope = batchMode ? "batch RTT" : "1 msg/round";
    const char* pctLabel = batchMode ? "batch" : "rtt";

    // 每线程 QPS：线程少时逐个列出，便于判断扩展性。
    // 注意：分母是各线程自己的 activeUs，和总 QPS 的门闩窗口不同源，
    // 两者不能相加，也不会严格相等。
    if (threads <= 32) {
        std::printf("per-thread QPS (active time) :");
        for (const ThreadResult& r : results) {
            const double activeSec = static_cast<double>(r.activeUs) / 1e6;
            if (activeSec > 0.0) {
                std::printf(" %.1f",
                            static_cast<double>(r.completed) / activeSec);
            } else {
                std::printf(" n/a");
            }
        }
        std::printf("\n");
    }

    const double minUs =
        latencies.empty() ? 0.0 : static_cast<double>(latencies.front());
    const double avgUs =
        latencies.empty()
            ? 0.0
            : static_cast<double>(latencySumUs) /
                  static_cast<double>(latencies.size());

    std::printf("samples         : %zu rounds (%s, %llu failed%s)    "
                "max : %.0f us\n",
                latencies.size(), scope,
                static_cast<unsigned long long>(failedRounds),
                sampleCapped ? ", capped" : "",
                latencies.empty()
                    ? 0.0
                    : static_cast<double>(latencies.back()));
    std::printf("min (%s)     : %.0f us\n", pctLabel, minUs);
    std::printf("avg (%s)     : %.0f us\n", pctLabel, avgUs);
    std::printf("P50 (%s)     : %.0f us\n", pctLabel,
                percentile(latencies, 50.0));
    std::printf("P99 (%s)     : %.0f us\n", pctLabel,
                percentile(latencies, 99.0));
    std::printf("P999 (%s)    : %.0f us\n", pctLabel,
                percentile(latencies, 99.9));

    // 退出码分级，方便脚本判断是否还能继续：
    //   0 = 无任何失败
    //   1 = 参数错误（见前面的 return 1）
    //   2 = 有失败，但仍有可用数据（completed > 0）
    //   3 = 完全失败（没有可用连接或没有完成任何请求）
    const bool allGood = connected == attempted && timeouts == 0 &&
                         ioErrors == 0 && failedRounds == 0;
    if (allGood) {
        return 0;
    }
    if (connected == 0 || completed == 0) {
        return 3;
    }
    return 2;
}
