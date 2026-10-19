// test_inetaddress.cc —— Day 19：InetAddress / SocketsOps / Socket
//
// 覆盖：
//   · 端口字节序（最容易写错的地方）
//   · 三种构造 + toIp / toIpPort
//   · createNonblockingOrDie 的两个标志位（NONBLOCK / CLOEXEC）
//   · Socket 的 RAII（析构自动 close）
//   · accept 返回的 connfd 也是非阻塞的

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include <gtest/gtest.h>

#include "muduo/net/InetAddress.h"
#include "muduo/net/Socket.h"
#include "muduo/net/SocketsOps.h"

using namespace muduo::net;

// ---------------------------------------------------------------------------
// InetAddress
// ---------------------------------------------------------------------------

TEST(InetAddressTest, PortIsNetworkByteOrder) {
    InetAddress addr(8080);

    // ★ 构造时做了 hostToNetwork16，toPort() 再转回来 —— 两边对称才相等。
    //   漏掉任何一次转换，这里都会得到一个巨大的数（比如 0x901f0000）。
    EXPECT_EQ(8080, addr.toPort());
    EXPECT_EQ("0.0.0.0", addr.toIp());
    EXPECT_EQ("0.0.0.0:8080", addr.toIpPort());
}

TEST(InetAddressTest, LoopbackOnly) {
    InetAddress addr(9000, /*loopbackOnly=*/true);

    EXPECT_EQ("127.0.0.1", addr.toIp());
    EXPECT_EQ(9000, addr.toPort());
    EXPECT_EQ("127.0.0.1:9000", addr.toIpPort());
}

TEST(InetAddressTest, FromIpAndPort) {
    InetAddress addr("127.0.0.1", 12345);

    EXPECT_EQ("127.0.0.1", addr.toIp());
    EXPECT_EQ(12345, addr.toPort());
    EXPECT_EQ("127.0.0.1:12345", addr.toIpPort());
    EXPECT_EQ(AF_INET, addr.family());
}

// ---------------------------------------------------------------------------
// SocketsOps
// ---------------------------------------------------------------------------

TEST(SocketsOpsTest, CreateNonblockingOrDieSetsFlags) {
    int fd = sockets::createNonblockingOrDie();
    ASSERT_GE(fd, 0);

    // O_NONBLOCK 写在 socket() 的 type 参数里也有同样的效果
    int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(0, flags & O_NONBLOCK);

    // FD_CLOEXEC 必须在创建时就设好，否则 fork/exec 之间会有继承窗口
    int fdFlags = ::fcntl(fd, F_GETFD, 0);
    ASSERT_GE(fdFlags, 0);
    EXPECT_NE(0, fdFlags & FD_CLOEXEC);

    sockets::close(fd);
}

TEST(SocketsOpsTest, AcceptReturnsNonblockingConnfd) {
    int listenfd = sockets::createNonblockingOrDie();
    ASSERT_GE(listenfd, 0);

    InetAddress local(0, /*loopbackOnly=*/true);   // 端口 0：内核分配
    sockets::bindOrDie(listenfd, local.getSockAddr());
    sockets::listenOrDie(listenfd);

    // 拿到内核实际分配的端口
    struct sockaddr_in bound = sockets::getLocalAddr(listenfd);
    InetAddress boundAddr(bound);
    ASSERT_NE(0, boundAddr.toPort());

    // 裸 socket 连上来（客户端不需要非阻塞）
    int client = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    ASSERT_EQ(0, ::connect(client, boundAddr.getSockAddr(),
                           static_cast<socklen_t>(sizeof(struct sockaddr_in))));

    struct sockaddr_in peer;
    int connfd = sockets::accept(listenfd, &peer);
    ASSERT_GE(connfd, 0);

    // accept4 的作用：新 fd 直接就是非阻塞 + CLOEXEC
    int flags = ::fcntl(connfd, F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(0, flags & O_NONBLOCK);

    // 对端地址 = 客户端本地地址
    struct sockaddr_in clientLocal;
    socklen_t len = static_cast<socklen_t>(sizeof(clientLocal));
    ASSERT_EQ(0, ::getsockname(client,
                               reinterpret_cast<struct sockaddr*>(&clientLocal),
                               &len));
    EXPECT_EQ(InetAddress(clientLocal).toPort(), InetAddress(peer).toPort());
    EXPECT_EQ(InetAddress(clientLocal).toIp(), InetAddress(peer).toIp());

    sockets::close(connfd);
    ::close(client);
    sockets::close(listenfd);
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

TEST(SocketTest, DestructorClosesFd) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(fd, 0);

    {
        Socket s(fd);
        EXPECT_EQ(fd, s.fd());
    }

    // RAII 生效：fd 已经被关掉了
    errno = 0;
    EXPECT_EQ(-1, ::fcntl(fd, F_GETFD, 0));
    EXPECT_EQ(EBADF, errno);
}

TEST(SocketTest, SetOptionsDoNotCrash) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    Socket s(fd);

    // 这几个在普通 socket 上都应该成功；失败会打日志，但不会崩
    s.setReuseAddr(true);
    s.setTcpNoDelay(true);
    s.setKeepAlive(true);

    int optval = 0;
    socklen_t len = static_cast<socklen_t>(sizeof(optval));
    ASSERT_EQ(0, ::getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, &len));
    EXPECT_EQ(1, optval);

    ASSERT_EQ(0, ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, &len));
    EXPECT_EQ(1, optval);
}
