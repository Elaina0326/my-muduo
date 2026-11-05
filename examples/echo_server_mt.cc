// echo_server_mt.cc —— 多线程 echo 服务器（压测与扩展性验证用）
//
// 与 examples/echo_server.cc 的分工：
//   echo_server.cc     手写 Acceptor + Channel，单线程，用于单独验证底层组件；
//   echo_server_mt.cc  走完整的 TcpServer + EventLoopThreadPool，用于压测和
//                      多核扩展性对比。
//
// 运行：
//   ./build-rel/echo_server_mt 8888 0     # 0 个 IO 线程：全部在 base loop 处理
//   ./build-rel/echo_server_mt 8888 4     # 4 个 IO 线程
//
// 压测请用 Release 构建（Debug 的数字没有意义）。

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "muduo/base/Logging.h"
#include "muduo/net/Buffer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/TcpServer.h"

using namespace muduo;
using namespace muduo::net;

namespace
{

// 严格解析非负整数，失败返回 -1（与 benchmarks/bench_client.cc 同一写法）
long long parseNonNegative(const char* s)
{
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0)
    {
        return -1;
    }
    return v;
}

constexpr int kMaxIOThreads = 64;

// 输出缓冲超过这个值就断开，防止慢消费者把内存吃光。
// 没有它时，一个只发不读的客户端能让 outputBuffer 无限增长
// （实测 64MB 负载可把 RSS 从 8MB 推到 100MB 以上）。
//
// 注意这是「每连接」上限：N 条慢连接最坏占用 N × 8MB。
constexpr size_t kHighWaterMark = 8 * 1024 * 1024;

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "usage: %s [port] [io_threads]\n"
                 "  port       : 1..65535，默认 8888\n"
                 "  io_threads : 0..%d，0 表示全部在 base loop 处理，默认 0\n"
                 "example: %s 8888 4\n",
                 prog, kMaxIOThreads, prog);
}

}  // namespace

int main(int argc, char* argv[])
{
    // 对端提前关闭时 write 会触发 SIGPIPE，默认动作是杀进程
    ::signal(SIGPIPE, SIG_IGN);

    long long portValue = 8888;
    long long threadValue = 0;

    if (argc > 1)
    {
        portValue = parseNonNegative(argv[1]);
        if (portValue < 1 || portValue > 65535)
        {
            std::fprintf(stderr, "port 必须是 1..65535 的整数，收到 \"%s\"\n",
                         argv[1]);
            usage(argv[0]);
            return 1;
        }
    }
    if (argc > 2)
    {
        threadValue = parseNonNegative(argv[2]);
        if (threadValue < 0 || threadValue > kMaxIOThreads)
        {
            std::fprintf(stderr, "io_threads 必须是 0..%d 的整数，收到 \"%s\"\n",
                         kMaxIOThreads, argv[2]);
            usage(argv[0]);
            return 1;
        }
    }
    if (argc > 3)
    {
        std::fprintf(stderr, "多余参数: \"%s\"\n", argv[3]);
        usage(argv[0]);
        return 1;
    }

    const uint16_t port = static_cast<uint16_t>(portValue);
    const int threadNum = static_cast<int>(threadValue);

    EventLoop loop;
    TcpServer server(&loop, InetAddress(port), "echo-mt");

    // threadNum == 0：线程池不创建线程，getNextLoop() 回落到 baseLoop，
    //                    所有连接都在主线程处理（等价单线程）。
    // threadNum >  0：创建 threadNum 个 EventLoopThread，新连接按 round-robin
    //                    分给它们，每条连接的事件只在自己的 loop 线程里处理。
    server.setThreadNum(threadNum);

    // 慢消费者保护：outputBuffer 跨过 kHighWaterMark 就断开这条连接。
    //
    // 实际触发链（两处「延后」是安全的前提，缺一不可）：
    //   sendInLoop → notifyHighWaterMark → queueInLoop(水位回调)      [延后 1]
    //     → doPendingFunctors → forceClose → forceCloseInLoop → handleClose
    //       此时已不在 sendInLoop 的栈上：disableAll 之后不会再有 enableWriting
    //     → closeCallback → removeConnectionInLoop
    //     → queueInLoop(connectDestroyed) → channel_->remove()        [延后 2]
    //
    // 回调里再套一层 queueInLoop 是显式的防御：不依赖 notifyHighWaterMark
    // 内部恰好用了 queueInLoop 这个实现细节。将来它若改成同步调用，
    // handleClose 也不会被塞进 sendInLoop 的栈里。
    server.setConnectionCallback(
        [](const TcpConnectionPtr& conn) {
            if (!conn->connected())
            {
                return;
            }
            conn->setHighWaterMarkCallback(
                [](const TcpConnectionPtr& c, size_t len) {
                    LOG_WARN << "输出缓冲 " << len << " 字节超过水位，断开 "
                             << c->name() << " (" << c->peerAddress().toIpPort()
                             << ")";
                    c->getLoop()->queueInLoop([c] { c->forceClose(); });
                },
                kHighWaterMark);
        });

    // 原样回显；send(Buffer*) 内部会 retrieveAll，不需要手动清空
    server.setMessageCallback(
        [](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            conn->send(buf);
        });

    server.start();
    LOG_INFO << "echo_server_mt 启动，端口 " << port << "，IO 线程 " << threadNum;

    loop.loop();
    return 0;
}
