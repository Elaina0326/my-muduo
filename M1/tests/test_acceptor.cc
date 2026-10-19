// test_acceptor.cc —— Day 21：Acceptor 接受新连接
//
// 覆盖：
//   · bind + listen 后 listenning() 为真
//   · accept 后回调拿到 connfd 和正确的对端地址
//   · 多个连接都能被接受
//   · 没设置回调时不崩、listen socket 不被破坏

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include "muduo/net/Acceptor.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/EventLoopThread.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/SocketsOps.h"

using namespace muduo::net;

namespace
{

// 把一段代码投递到 loop 线程执行，并等它跑完。
// Acceptor 的 listen() / 析构都会碰 Channel，必须在 loop 线程里做。
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

int connectTo(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    InetAddress addr("127.0.0.1", port);
    if (::connect(fd, addr.getSockAddr(),
                  static_cast<socklen_t>(sizeof(struct sockaddr_in))) < 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

uint16_t localPort(int fd)
{
    struct sockaddr_in local;
    socklen_t len = static_cast<socklen_t>(sizeof(local));
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &len);
    return InetAddress(local).toPort();
}

class AcceptorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        base_ = baseThread_.startLoop();
        ASSERT_NE(base_, nullptr);
    }

    void TearDown() override
    {
        // 析构要 disableAll + remove，同样必须在 loop 线程里
        runInLoopAndWait(base_, [this] { acceptor_.reset(); });
    }

    // ★ 绑端口 0：让内核分配，避免固定端口冲突。
    //   实际端口通过 listenFd() + getsockname 拿回来。
    void makeAcceptor()
    {
        acceptor_.reset(new Acceptor(base_, InetAddress(0, true), false));
    }

    uint16_t port() const
    {
        struct sockaddr_in addr = sockets::getLocalAddr(acceptor_->listenFd());
        return InetAddress(addr).toPort();
    }

    EventLoopThread baseThread_;
    EventLoop* base_ = nullptr;
    std::unique_ptr<Acceptor> acceptor_;
};

}  // namespace

TEST_F(AcceptorTest, BindAndListen)
{
    bool listening = false;
    uint16_t listenPort = 0;

    runInLoopAndWait(base_, [&]
                     {
                         makeAcceptor();
                         acceptor_->listen();
                         listening = acceptor_->listenning();
                         listenPort = port();
                     });

    EXPECT_TRUE(listening);
    EXPECT_NE(0, listenPort);

    // 能连上就说明 bind + listen 真的生效了
    int c = connectTo(listenPort);
    ASSERT_GE(c, 0);
    ::close(c);
}

TEST_F(AcceptorTest, AcceptsConnectionAndReportsPeer)
{
    std::promise<std::pair<int, uint16_t>> accepted;
    auto future = accepted.get_future();
    uint16_t listenPort = 0;

    runInLoopAndWait(base_, [&]
                     {
                         makeAcceptor();
                         acceptor_->setNewConnectionCallback(
                             [&accepted](int fd, const InetAddress& peer)
                             {
                                 accepted.set_value({fd, peer.toPort()});
                             });
                         acceptor_->listen();
                         listenPort = port();
                     });

    int c = connectTo(listenPort);
    ASSERT_GE(c, 0);

    auto result = future.get();
    EXPECT_GE(result.first, 0);
    // accept 拿到的对端端口，必须等于客户端的本地端口
    EXPECT_EQ(localPort(c), result.second);

    ::close(result.first);
    ::close(c);
}

TEST_F(AcceptorTest, MultipleConnections)
{
    std::promise<void> allAccepted;
    auto future = allAccepted.get_future();
    uint16_t listenPort = 0;

    // 放在外面，测试结束时统一关闭，避免 fd 泄漏
    auto count = std::make_shared<std::atomic<int>>(0);
    auto fds = std::make_shared<std::vector<int>>();

    runInLoopAndWait(base_, [&]
                     {
                         makeAcceptor();
                         acceptor_->setNewConnectionCallback(
                             [count, fds, &allAccepted](int fd, const InetAddress&)
                             {
                                 fds->push_back(fd);
                                 if (count->fetch_add(1) + 1 == 3)
                                 {
                                     allAccepted.set_value();
                                 }
                             });
                         acceptor_->listen();
                         listenPort = port();
                     });

    int c1 = connectTo(listenPort);
    int c2 = connectTo(listenPort);
    int c3 = connectTo(listenPort);
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);
    ASSERT_GE(c3, 0);

    future.wait();

    EXPECT_EQ(3u, fds->size());
    for (int fd : *fds)
    {
        ::close(fd);
    }

    ::close(c1);
    ::close(c2);
    ::close(c3);
}

TEST_F(AcceptorTest, NoCallbackStillAcceptsAndStaysAlive)
{
    uint16_t listenPort = 0;
    runInLoopAndWait(base_, [&]
                     {
                         makeAcceptor();
                         acceptor_->listen();
                         listenPort = port();
                     });

    // 没设 newConnectionCallback：accept 后应该直接 close，而不是崩溃/泄漏
    for (int i = 0; i < 5; ++i)
    {
        int c = connectTo(listenPort);
        ASSERT_GE(c, 0);
        ::close(c);
    }

    // listen socket 仍然可用
    int c = connectTo(listenPort);
    ASSERT_GE(c, 0);
    ::close(c);
}
