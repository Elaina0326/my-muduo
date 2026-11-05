// test_idletimeout.cc —— 连接空闲超时的回归测试
//
// 这里护的是四个设计决策，每一个写错都能被某一个用例抓到：
//
//   ① 静默连接会被关闭（基础行为）
//   ② 持续收到数据不会被误杀（续期正确）
//   ③ 只写不读【仍然】会被关闭（证明"只统计读"这个决策真的落地了）
//   ④ 定时器回调持有 weak_ptr，不延长连接生命周期（防内存泄漏）
//
// 另外两条边界：
//   ⑤ 不调用 enableIdleTimeout 时永远不超时（默认关闭）
//   ⑥ enableIdleTimeout(0) 能取消已经挂上的定时器
//
// 时间成本：每个用例等待 1.3~2.5 秒真实时间，全部跑完约 8 秒。
// 之所以用真实时间而不是 mock 时钟：这块代码的坑全在"定时器与事件循环的
// 交互"上（回调在哪个栈里执行、取消时机），mock 掉时间就测不出来了。

#include "muduo/net/Buffer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>

namespace
{

using namespace muduo::net;

// 与 test_tcpconnection.cc 里的同名函数保持一致：
// socketpair 的 fd[0] 交给 TcpConnection，fd[1] 充当"对端"。
// 回调必须全部设置，否则 handleClose() 会调用空的 std::function。
std::shared_ptr<TcpConnection> makeConnection(EventLoop *loop, int fd)
{
    auto conn = std::make_shared<TcpConnection>(loop, "idle-test", fd,
                                                InetAddress(), InetAddress());
    conn->setConnectionCallback([](const TcpConnectionPtr &) {});
    conn->setMessageCallback(
        [](const TcpConnectionPtr &, Buffer *, muduo::Timestamp) {});
    conn->setCloseCallback([](const TcpConnectionPtr &) {});
    conn->connectEstablished();
    return conn;
}

// 让连接走完关闭流程并从 poller 摘除，避免析构时留下悬垂 Channel。
void tearDown(EventLoop *loop, const std::shared_ptr<TcpConnection> &conn)
{
    loop->assertInLoopThread();
    conn->forceClose();
    conn->connectDestroyed();
}

// 打开一对 socketpair。返回 bool 而不是在函数里 ASSERT：
// gtest 的 ASSERT_* 只能中止当前函数，放在这里会让调用方拿着未初始化的 fd 继续跑。
bool makePair(int fds[2])
{
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ① 静默连接：1 秒没有任何数据进来 → 被关闭
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, ClosesSilentConnection)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);
    std::atomic<bool> closed{false};
    conn->setCloseCallback([&](const TcpConnectionPtr &) { closed = true; });

    conn->enableIdleTimeout(1);

    // 1.5 秒：超时 1 秒 + 0.5 秒容错（调度抖动、慢机器）
    loop.runAfter(1.5, [&] { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(closed.load()) << "静默连接应在 1 秒后被关闭";
    EXPECT_TRUE(conn->disconnected());

    tearDown(&loop, conn);
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ② 持续收数据：每 300ms 写一个字节，2.4 秒内不该被踢
//
// 这条同时验证了"惰性判定"的正确性：读事件只更新 lastActiveTime_，
// 定时器到点时发现没超阈值就重新等剩余时间。如果惰性判定写错
// （比如忘了重新 arm），连接会在第一个超时点就被关掉。
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, KeepsActiveConnection)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);
    std::atomic<bool> closed{false};
    conn->setCloseCallback([&](const TcpConnectionPtr &) { closed = true; });

    conn->enableIdleTimeout(1);

    const char byte = 'x';
    // 不保存 TimerId：loop 停止后 cancel 不会执行（runInLoop 只是入队），
    // 而 loop 停下后定时器本来也不会再触发。
    loop.runEvery(0.3, [&] {
        const ssize_t n = ::write(fds[1], &byte, 1);
        (void)n;
    });

    loop.runAfter(2.5, [&] { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(closed.load()) << "持续有数据进来，不该被判定为空闲";
    EXPECT_TRUE(conn->connected());

    tearDown(&loop, conn);
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ③ 只写不读：服务端一直在发，对端一个字都不读 → 仍然要被关闭
//
// 这是整套设计里最关键的一条。如果把"写"也算成活跃，这个用例会失败。
// 实际场景：一条正在传大文件的连接，服务端持续 write、客户端读得慢，
// 它不该被判定成死连接。
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, WriteDoesNotKeepAlive)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);
    std::atomic<bool> closed{false};
    conn->setCloseCallback([&](const TcpConnectionPtr &) { closed = true; });

    conn->enableIdleTimeout(1);

    // 每 200ms 发 1KB，但对端从不 read —— 连接是"只写不读"的
    loop.runEvery(0.2, [&] {
        conn->send(std::string(1024, 'y'));
    });

    loop.runAfter(1.5, [&] { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(closed.load()) << "只有写、没有读，仍应被判为空闲";

    tearDown(&loop, conn);
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ④ 定时器不延长连接生命周期
//
// 比较 enableIdleTimeout 前后的引用计数。如果回调捕获的是 shared_ptr，
// 计数会 +1，连接永远不会析构 —— 超时功能自己成了内存泄漏源。
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, TimerDoesNotHoldStrongReference)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);

    const long before = conn.use_count();
    conn->enableIdleTimeout(60); // 长超时，定时器会一直挂着
    const long after = conn.use_count();

    EXPECT_EQ(before, after)
        << "挂定时器后引用计数不该增加（说明回调捕获的是 weak_ptr，"
        << "before=" << before << " after=" << after << "）";

    tearDown(&loop, conn);
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ⑤ 默认关闭：不调用 enableIdleTimeout，等 1.5 秒也不该被踢
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, DisabledByDefault)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);
    std::atomic<bool> closed{false};
    conn->setCloseCallback([&](const TcpConnectionPtr &) { closed = true; });

    // 刻意不调用 enableIdleTimeout

    loop.runAfter(1.5, [&] { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(closed.load()) << "没启用超时时不该关闭连接";
    EXPECT_TRUE(conn->connected());

    tearDown(&loop, conn);
    ::close(fds[1]);
}

// ---------------------------------------------------------------------------
// ⑥ 关闭超时：先挂 1 秒超时，再调 enableIdleTimeout(0) 取消，
//    2 秒后连接应当仍然活着
//
// 这条护的是 armIdleTimer 里"先 cancel 旧定时器"那段：
// 如果 cancel 没生效，旧的 1 秒定时器照样会触发关闭。
// ---------------------------------------------------------------------------
TEST(IdleTimeoutTest, ZeroCancelsArmedTimer)
{
    EventLoop loop;
    int fds[2];
    ASSERT_TRUE(makePair(fds));

    auto conn = makeConnection(&loop, fds[0]);
    std::atomic<bool> closed{false};
    conn->setCloseCallback([&](const TcpConnectionPtr &) { closed = true; });

    conn->enableIdleTimeout(1);
    conn->enableIdleTimeout(0); // 取消

    loop.runAfter(2.0, [&] { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(closed.load()) << "调用 enableIdleTimeout(0) 后不该再触发超时";
    EXPECT_TRUE(conn->connected());

    tearDown(&loop, conn);
    ::close(fds[1]);
}
