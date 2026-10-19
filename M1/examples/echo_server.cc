// echo_server.cc —— Day 21：第一个能跑的 echo 服务器
//
// 目的：不依赖 TcpConnection，只用
//   EventLoop + Channel + Acceptor + Buffer
// 拼出一个完整的服务器，验证前几周的组件能协同工作。
//
// 运行：
//   ./build/echo_server 8888
//
// 测试：
//   printf 'hello\n' | nc 127.0.0.1 8888
//
// ★ 和 Day 22 的关系：
//   这个文件里的每一件事（读、写、EAGAIN 时挂起、对端 FIN 时冲刷、关闭时反注册）
//   后面都会被 TcpConnection 用更完整的方式实现一遍。
//   先手写一遍，才知道 TcpConnection 那些成员是为什么存在的。

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <map>
#include <memory>

#include "muduo/base/Logging.h"
#include "muduo/net/Acceptor.h"
#include "muduo/net/Buffer.h"
#include "muduo/net/Channel.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/Socket.h"

using namespace muduo;
using namespace muduo::net;

namespace
{

// ---------------------------------------------------------------------------
// 一条连接需要的三样东西
//
// ★ 成员顺序：Socket 在前、Channel 在后 —— 析构是逆序，
//   所以 Channel 先析构（反注册），Socket 后析构（close fd）。
// ---------------------------------------------------------------------------
struct EchoConn
{
    EchoConn(EventLoop* loop, int fd) : socket(fd), channel(loop, fd) {}

    Socket socket;
    Channel channel;
    Buffer buffer;

    // 对端已经半关闭（读到 FIN），等缓冲里的数据发完再关
    bool peerClosed = false;

    // 已经在关闭流程里 —— 防止同一批事件里再次进入
    bool closing = false;
};

// ---------------------------------------------------------------------------
// EchoServer
// ---------------------------------------------------------------------------
class EchoServer
{
public:
    EchoServer(EventLoop* loop, const InetAddress& listenAddr)
        : loop_(loop), acceptor_(loop, listenAddr, false)
    {
        acceptor_.setNewConnectionCallback(
            [this](int fd, const InetAddress& peer) { newConnection(fd, peer); });
    }

    void start() { acceptor_.listen(); }

private:
    void newConnection(int fd, const InetAddress& peer)
    {
        auto conn = std::make_shared<EchoConn>(loop_, fd);
        conns_[fd] = conn;

        // ★ loop 在分发事件期间持有 shared_ptr。
        //   这样回调里把 conns_ 里的引用删掉也不会 UAF。
        conn->channel.tie(conn);

        EchoConn* raw = conn.get();
        conn->channel.setReadCallback(
            [this, fd, raw](Timestamp) { handleRead(fd, raw); });
        conn->channel.setWriteCallback([this, fd, raw] { handleWrite(fd, raw); });
        conn->channel.enableReading();

        LOG_INFO << "新连接 " << peer.toIpPort() << " (fd=" << fd << ")";
    }

    // ---------------- 读 ----------------
    void handleRead(int fd, EchoConn* conn)
    {
        if (conn->closing)
        {
            return;
        }

        int savedErrno = 0;
        ssize_t n = conn->buffer.readFd(fd, &savedErrno);

        if (n > 0)
        {
            writeAll(fd, conn);
        }
        else if (n == 0)
        {
            // ★ 对端 FIN：不能立刻 close —— 缓冲里可能还有没发完的数据
            conn->peerClosed = true;
            conn->channel.disableReading();
            writeAll(fd, conn);
        }
        else if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK &&
                 savedErrno != EINTR)
        {
            LOG_SYSERR << "Buffer::readFd";
            closeConn(fd, conn);
        }
    }

    // ---------------- 写 ----------------
    void handleWrite(int fd, EchoConn* conn)
    {
        if (conn->closing)
        {
            return;
        }
        writeAll(fd, conn);
    }

    void writeAll(int fd, EchoConn* conn)
    {
        while (conn->buffer.readableBytes() > 0)
        {
            ssize_t n = ::write(fd, conn->buffer.peek(),
                                conn->buffer.readableBytes());
            if (n > 0)
            {
                conn->buffer.retrieve(static_cast<size_t>(n));
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                // 内核发送缓冲区满：剩下的等 EPOLLOUT，不再空转重试
                conn->channel.enableWriting();
                return;
            }
            else
            {
                if (n < 0)
                {
                    LOG_SYSERR << "::write";
                }
                closeConn(fd, conn);
                return;
            }
        }

        conn->channel.disableWriting();

        // 对端已关闭，且数据发完了 → 现在可以真正关闭
        if (conn->peerClosed)
        {
            closeConn(fd, conn);
        }
    }

    // ---------------- 关闭 ----------------
    void closeConn(int fd, EchoConn* conn)
    {
        if (conn->closing)
        {
            return;
        }
        conn->closing = true;

        auto it = conns_.find(fd);
        if (it == conns_.end())
        {
            return;
        }

        conn->channel.disableAll();
        conn->channel.remove();     // 从 Poller 摘掉，避免悬垂指针
        conns_.erase(it);           // tie_ 还持有引用，对象不会在这里销毁
    }

    EventLoop* loop_;
    Acceptor acceptor_;
    std::map<int, std::shared_ptr<EchoConn>> conns_;
};

}  // namespace

int main(int argc, char* argv[])
{
    // 对端提前关闭时 write 会触发 SIGPIPE —— 默认动作是杀进程
    ::signal(SIGPIPE, SIG_IGN);

    uint16_t port = 8888;
    if (argc > 1)
    {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    EventLoop loop;
    InetAddress listenAddr(port);
    EchoServer server(&loop, listenAddr);

    server.start();
    LOG_INFO << "echo_server 启动，端口 " << port;

    loop.loop();
    return 0;
}
