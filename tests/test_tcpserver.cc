// test_tcpserver.cc —— TcpServer 的端到端用例
//
// 覆盖：
//   ① 建连后 connectionCallback 恰好被调用一次
//   ② 对端断开后连接对象能被释放（配合 ASan 跑）
//   ③ 多 IO 线程并发连接，消息不串
//   ④ 销毁 server 后，同一端口能立刻重新 bind

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "muduo/base/Timestamp.h"
#include "muduo/net/Buffer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/EventLoopThread.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/TcpConnection.h"
#include "muduo/net/TcpServer.h"

using namespace muduo::net;

namespace
{

// 投递到 loop 线程并等它跑完（TcpServer 的构造/析构都要求 loop 线程）。
template <typename Fn>
void runInLoopAndWait(EventLoop* loop, Fn fn)
{
    std::promise<void> done;
    auto future = done.get_future();
    loop->runInLoop([&fn, &done]
                    {
                        fn();
                        done.set_value();
                    });
    future.wait();
}

// 绑 0 号端口拿一个空闲端口，再关掉给 TcpServer 用。
uint16_t freePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// acceptor 的 listen 是投递到 loop 线程的，这里带重试。
int connectTo(uint16_t port)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return -1;
        }
        InetAddress addr("127.0.0.1", port);
        if (::connect(fd, addr.getSockAddr(),
                      static_cast<socklen_t>(sizeof(struct sockaddr_in))) == 0)
        {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

bool sendAll(int fd, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string recvExact(int fd, size_t len)
{
    std::string out(len, '\0');
    size_t got = 0;
    while (got < len)
    {
        const ssize_t n = ::recv(fd, &out[got], len - got, 0);
        if (n <= 0)
        {
            break;
        }
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    return out;
}

class TcpServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        loop_ = thread_.startLoop();
        ASSERT_NE(loop_, nullptr);
    }

    EventLoopThread thread_;
    EventLoop* loop_ = nullptr;
};

} // namespace

// ① 建连后 connectionCallback 恰好一次
TEST_F(TcpServerTest, ConnectionCallbackFiresOnce)
{
    const uint16_t port = freePort();
    auto server = std::make_unique<TcpServer>(loop_, InetAddress(port), "t1");

    std::atomic<int> connectedCalls{0};
    std::promise<void> first;
    auto firstFuture = first.get_future();

    server->setConnectionCallback(
        [&](const TcpConnectionPtr& conn)
        {
            if (conn->connected() && connectedCalls.fetch_add(1) == 0)
            {
                first.set_value();
            }
        });

    runInLoopAndWait(loop_, [&]
                     { server->start(); });
    const int fd = connectTo(port);
    ASSERT_GE(fd, 0);

    ASSERT_EQ(firstFuture.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);

    // 留时间给可能出现的重复回调
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(connectedCalls.load(), 1)
        << "connectionCallback 被调用了多次（看 TcpServer::newConnection 是否重复 connectEstablished）";

    ::close(fd);
    runInLoopAndWait(loop_, [&]
                     { server.reset(); });
}

// ② 对端断开后连接对象能释放
TEST_F(TcpServerTest, ConnectionReleasedAfterPeerClose)
{
    const uint16_t port = freePort();
    auto server = std::make_unique<TcpServer>(loop_, InetAddress(port), "t2");

    std::weak_ptr<TcpConnection> weak;
    std::atomic<bool> hasConn{false};
    std::atomic<bool> closedOnce{false};
    std::promise<void> closed;
    auto closedFuture = closed.get_future();

    server->setConnectionCallback(
        [&](const TcpConnectionPtr& conn)
        {
            if (conn->connected())
            {
                weak = conn;
                hasConn = true;
            }
            else if (!closedOnce.exchange(true))
            {
                closed.set_value();
            }
        });

    runInLoopAndWait(loop_, [&]
                     { server->start(); });
    const int fd = connectTo(port);
    ASSERT_GE(fd, 0);

    for (int i = 0; i < 200 && !hasConn.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(hasConn.load());

    ::close(fd); // 对端断开

    ASSERT_EQ(closedFuture.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);

    // removeConnectionInLoop 把 connectDestroyed 排队后，最后一个引用才会释放
    for (int i = 0; i < 200 && !weak.expired(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(weak.expired()) << "连接对象没有被释放（泄漏，配合 ASan 跑更直观）";

    runInLoopAndWait(loop_, [&]
                     { server.reset(); });
}

// ③ 多 IO 线程并发连接，消息不串
TEST_F(TcpServerTest, MultiIoThreadsNoCrossTalk)
{
    const uint16_t port = freePort();
    auto server = std::make_unique<TcpServer>(loop_, InetAddress(port), "t3");
    server->setThreadNum(4);
    server->setMessageCallback(
        [](const TcpConnectionPtr& conn, Buffer* buf, muduo::Timestamp)
        {
            conn->send(buf->retrieveAllAsString());
        });
    runInLoopAndWait(loop_, [&]
                     { server->start(); });

    constexpr int kClients = 16;
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);

    for (int i = 0; i < kClients; ++i)
    {
        clients.emplace_back(
            [&, i]
            {
                const int fd = connectTo(port);
                if (fd < 0)
                {
                    return;
                }
                std::string payload = "client-" + std::to_string(i) + "-";
                payload.resize(4096, static_cast<char>('a' + i % 26));

                if (sendAll(fd, payload) &&
                    recvExact(fd, payload.size()) == payload)
                {
                    ++ok;
                }
                ::close(fd);
            });
    }
    for (auto& t : clients)
    {
        t.join();
    }

    EXPECT_EQ(ok.load(), kClients) << "有连接串了数据或回显不完整";

    runInLoopAndWait(loop_, [&]
                     { server.reset(); });
}

// ④ 销毁 server 后端口能立刻重新 bind
TEST_F(TcpServerTest, RebindAfterServerDestroyed)
{
    const uint16_t port = freePort();

    auto server1 = std::make_unique<TcpServer>(loop_, InetAddress(port), "t4a");
    runInLoopAndWait(loop_, [&]
                     { server1->start(); });
    const int fd1 = connectTo(port);
    ASSERT_GE(fd1, 0);
    ::close(fd1);

    runInLoopAndWait(loop_, [&]
                     { server1.reset(); });

    // 同一个端口立刻重建：listen fd 没关干净的话这里会 EADDRINUSE
    auto server2 = std::make_unique<TcpServer>(loop_, InetAddress(port), "t4b");
    runInLoopAndWait(loop_, [&]
                     { server2->start(); });
    const int fd2 = connectTo(port);
    EXPECT_GE(fd2, 0) << "端口没有释放（EADDRINUSE）";
    if (fd2 >= 0)
    {
        ::close(fd2);
    }

    runInLoopAndWait(loop_, [&]
                     { server2.reset(); });
}
