// test_tcpconnection.cc —— TcpConnection 的回归测试
//
// 这里护的是两个曾经真实出过问题的路径：
//   ① sendMulti 的分片数超过 IOV_MAX：writev 返回 EINVAL，
//      原实现静默丢数据 + 空缓冲 EPOLLOUT 空转；
//   ② sendFile 挂起期间调用 send()：数据进了 outputBuffer_，
//      文件发完后被 disableWriting() 永久闷在缓冲里。

#include "muduo/net/Buffer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/TcpConnection.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{

    // 建一条本地连接对象：socketpair 的 fd[0] 交给 TcpConnection。
    // 回调必须全部设置，否则 handleClose() 会调用空的 std::function。
    std::shared_ptr<muduo::net::TcpConnection> makeConnection(
        muduo::net::EventLoop *loop, int fd)
    {
        using namespace muduo::net;

        auto conn = std::make_shared<TcpConnection>(loop, "test-conn", fd,
                                                    InetAddress(), InetAddress());
        conn->setConnectionCallback([](const TcpConnectionPtr &) {});
        conn->setMessageCallback(
            [](const TcpConnectionPtr &, Buffer *, muduo::Timestamp) {});
        conn->setCloseCallback([](const TcpConnectionPtr &) {});
        conn->connectEstablished();
        return conn;
    }

    // 让连接走完关闭流程并把自己从 poller 里摘掉，避免析构时留下悬垂 Channel。
    void tearDown(muduo::net::EventLoop *loop,
                  const std::shared_ptr<muduo::net::TcpConnection> &conn)
    {
        loop->assertInLoopThread();
        conn->forceClose();
        conn->connectDestroyed();
    }

} // namespace

// ---------------------------------------------------------------------------
// ① sendMulti 超过 IOV_MAX
// ---------------------------------------------------------------------------
TEST(TcpConnectionTest, SendMultiOverIovMax)
{
    using namespace muduo::net;

    int fds[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    EventLoop loop;
    auto conn = makeConnection(&loop, fds[0]);

    // 2000 个分片，超过 Linux 的 IOV_MAX(1024)
    std::vector<std::string> parts(2000, std::string(10, 'x'));
    const size_t kTotal = 2000 * 10;

    conn->sendMulti(parts);

    // 修复前 writev 直接 EINVAL，一个字节都收不到
    std::string out(kTotal, '\0');
    size_t got = 0;
    while (got < kTotal)
    {
        ssize_t n = ::read(fds[1], &out[got], kTotal - got);
        ASSERT_GT(n, 0) << "只收到 " << got << " 字节";
        got += static_cast<size_t>(n);
    }

    EXPECT_EQ(out, std::string(kTotal, 'x'));

    tearDown(&loop, conn);
    conn.reset();
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ② sendFile 挂起期间 send() 的数据不能被丢
// ---------------------------------------------------------------------------
TEST(TcpConnectionTest, SendFileThenSendFlushesBufferedData)
{
    using namespace muduo::net;

    int fds[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // ★ TcpConnection 假定 fd 是非阻塞的（真实连接由 accept4 创建）。
    //   这里是测试里手工造出来的 fd，必须自己设上 —— 否则 sendfile
    //   会在阻塞 fd 上一直等缓冲区腾空，直接把测试挂死。
    int flags = ::fcntl(fds[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(0, ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK));

    // 把发送缓冲压小，保证 sendfile 一定会 EAGAIN
    int sndbuf = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // 读端加超时：修复前 TAIL 永远不来，测试不能挂死
    timeval tv{2, 0};
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 造一个 256KB 的临时文件
    char path[] = "/tmp/tcpconn_sendfile_XXXXXX";
    int filefd = ::mkstemp(path);
    ASSERT_GE(filefd, 0);
    ::unlink(path);

    const size_t kFileSize = 256 * 1024;
    std::string content(kFileSize, 'F');
    ASSERT_EQ(static_cast<ssize_t>(content.size()),
              ::write(filefd, content.data(), content.size()));

    EventLoop loop;
    auto conn = makeConnection(&loop, fds[0]);

    // guard 负责关文件 fd —— 网络层会持有它直到文件发完
    auto guard = std::shared_ptr<void>(reinterpret_cast<void *>(1),
                                       [filefd](void *)
                                       { ::close(filefd); });

    conn->sendFile(filefd, 0, kFileSize, guard);
    conn->send("TAIL"); // ★ 文件还在发的时候追加普通数据

    std::atomic<size_t> received{0};
    std::thread reader([&]
                       {
        // 等 sendfile 在满的发送缓冲上进入 EAGAIN
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::string out(kFileSize + 4, '\0');
        size_t got = 0;
        while (got < out.size()) {
            ssize_t n = ::read(fds[1], &out[got], out.size() - got);
            if (n <= 0) {
                break;
            }
            got += static_cast<size_t>(n);
        }
        received.store(got);
        loop.quit(); });

    loop.loop();
    reader.join();

    EXPECT_EQ(received.load(), kFileSize + 4)
        << "文件发完后 outputBuffer_ 里的 TAIL 没有被发出去";

    tearDown(&loop, conn);
    conn.reset();
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ③ 高水位回调：只在"跨过水位"的那一次触发，缓冲仍高时不重复触发
//
// T4 的回归测试。要点是让"跨水位"发生在 sendMulti 的慢路径上
// （channel 已经在写 / outputBuffer_ 已有数据）—— 修复前那条路径
// 根本没有 notifyHighWaterMark()，缓冲堆到上限回调也不来。
// ---------------------------------------------------------------------------
TEST(TcpConnectionTest, HighWaterMarkFiresOnceWhenCrossed)
{
    using namespace muduo::net;

    int fds[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // 两端都压小，并且先把内核发送缓冲灌满：
    // 之后 TcpConnection 的 writev 必然 EAGAIN，数据只能进 outputBuffer_，
    // 下一次 sendMulti 就会走慢路径。
    int bufSize = 1024;
    ASSERT_EQ(0, ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize)));
    ASSERT_EQ(0, ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize)));

    int flags = ::fcntl(fds[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(0, ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK));

    const std::string filler(1024, 'f');
    for (;;)
    {
        const ssize_t n = ::write(fds[0], filler.data(), filler.size());
        if (n <= 0)
        {
            break; // EAGAIN：发送缓冲已满
        }
    }

    EventLoop loop;
    auto conn = makeConnection(&loop, fds[0]);

    std::atomic<int> calls{0};
    std::atomic<size_t> lastSize{0};
    conn->setHighWaterMarkCallback(
        [&calls, &lastSize](const TcpConnectionPtr &, size_t size)
        {
            ++calls;
            lastSize = size;
        },
        1024);

    // ① 1 字节：低于水位，不触发；但会让 channel 进入 writing 状态
    conn->sendMulti({std::string(1, 'a')});

    const std::vector<std::string> big{std::string(8192, 'a'), std::string(8192, 'b')};

    // ② 8 KB × 2：慢路径追加，跨过水位 → 恰好触发一次
    loop.runAfter(0.05, [&]
                  { conn->sendMulti(big); });

    // 快照：此时回调应已执行（queueInLoop 投递的 functor 在上一轮 doPendingFunctors 跑过）
    int callsAfterCrossing = 0;
    size_t sizeAfterCrossing = 0;
    loop.runAfter(0.20, [&]
                  {
        callsAfterCrossing = calls.load();
        sizeAfterCrossing = lastSize.load(); });

    // ③ 缓冲仍高于水位：再发一批，不应重复触发
    loop.runAfter(0.30, [&]
                  { conn->sendMulti(big); });

    loop.runAfter(0.40, [&]
                  { loop.quit(); });

    loop.loop();

    EXPECT_EQ(callsAfterCrossing, 1) << "跨过水位时应恰好触发一次（慢路径也不能漏）";
    EXPECT_GE(sizeAfterCrossing, 1024u) << "回调参数应 >= 水位";
    EXPECT_EQ(calls.load(), 1) << "缓冲仍高于水位时不应重复触发";

    tearDown(&loop, conn);
    conn.reset();
    ::close(fds[1]);
}
