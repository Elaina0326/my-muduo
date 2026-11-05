// test_buffer.cc —— Day 20：应用层缓冲区
//
// 覆盖：
//   · 三索引的基本语义（readable / writable / prependable）
//   · makeSpace 的两条分支：移动回收 vs 扩容
//   · prepend / shrink / retrieveAll
//   · findCRLF / findEOL（HTTP 解析的基础）
//   · ★ readFd：普通读取、extrabuf 溢出、EAGAIN

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "muduo/net/Buffer.h"

using namespace muduo::net;

// ---------------------------------------------------------------------------
// 基本读写
// ---------------------------------------------------------------------------
TEST(BufferTest, AppendAndRetrieve)
{
    Buffer b;
    EXPECT_EQ(0u, b.readableBytes());
    EXPECT_EQ(Buffer::kCheapPrepend, b.prependableBytes());

    b.append("hello", 5);
    EXPECT_EQ(5u, b.readableBytes());
    EXPECT_EQ("hello", std::string(b.peek(), b.readableBytes()));

    b.retrieve(2);
    EXPECT_EQ(3u, b.readableBytes());
    EXPECT_EQ("llo", std::string(b.peek(), b.readableBytes()));

    // 消费超过可读长度是安全行为：等价于 retrieveAll
    b.retrieve(999);
    EXPECT_EQ(0u, b.readableBytes());
    EXPECT_EQ(Buffer::kCheapPrepend, b.prependableBytes());
}

TEST(BufferTest, RetrieveAllResetsToPrepend)
{
    Buffer b;
    b.append("abc");
    b.retrieveAll();

    EXPECT_EQ(0u, b.readableBytes());
    // ★ 归位到 8，而不是 0 —— 保住 prepend 的预留空间
    EXPECT_EQ(Buffer::kCheapPrepend, b.prependableBytes());

    // 清空后还能继续写
    b.append("xy");
    EXPECT_EQ("xy", std::string(b.peek(), b.readableBytes()));
}

// ---------------------------------------------------------------------------
// ★ makeSpace 的两条分支
// ---------------------------------------------------------------------------
TEST(BufferTest, MakeSpaceMovesInsteadOfGrow)
{
    Buffer b(16);                // 容量 = 8 + 16 = 24
    b.append("abcdefghij", 10);  // writer = 18，writable = 6
    b.retrieve(5);               // reader = 13，可读 "fghij"

    // 需要 8 字节 > 6：触发 makeSpace
    // 可写 6 + 已读 13 = 19 >= 8 + 8 = 16 → 应该走"移动回收"，而不是扩容
    b.append("12345678", 8);

    EXPECT_EQ(Buffer::kCheapPrepend, b.prependableBytes());  // 移动后 reader 回到 8
    EXPECT_EQ(13u, b.readableBytes());
    EXPECT_EQ("fghij12345678", std::string(b.peek(), b.readableBytes()));
}

TEST(BufferTest, MakeSpaceGrows)
{
    Buffer b(16);
    std::string big(100, 'x');
    b.append(big);

    EXPECT_EQ(100u, b.readableBytes());
    EXPECT_EQ(big, std::string(b.peek(), b.readableBytes()));
}

// ---------------------------------------------------------------------------
// prepend / shrink
// ---------------------------------------------------------------------------
TEST(BufferTest, Prepend)
{
    Buffer b;
    b.append("world");
    b.prepend("hello ", 6);   // 利用可写区前面的预留空间

    EXPECT_EQ("hello world", std::string(b.peek(), b.readableBytes()));
}

TEST(BufferTest, ShrinkKeepsReadableData)
{
    Buffer b;
    b.append(std::string(100, 'a'));
    b.retrieve(90);
    ASSERT_EQ(10u, b.readableBytes());

    b.shrink(0);

    EXPECT_EQ(10u, b.readableBytes());
    EXPECT_EQ(std::string(10, 'a'), std::string(b.peek(), b.readableBytes()));
    EXPECT_EQ(Buffer::kCheapPrepend, b.prependableBytes());
}

// ---------------------------------------------------------------------------
// 查找：HTTP 解析用
// ---------------------------------------------------------------------------
TEST(BufferTest, FindCRLFAndEOL)
{
    Buffer b;
    b.append("abc\r\ndef", 8);

    const char* crlf = b.findCRLF();
    ASSERT_NE(crlf, nullptr);
    EXPECT_EQ(3, crlf - b.peek());

    const char* eol = b.findEOL();
    ASSERT_NE(eol, nullptr);
    EXPECT_EQ(4, eol - b.peek());

    // 从 eol 之后开始找，已经没有 \r\n 了
    EXPECT_EQ(nullptr, b.findCRLF(b.peek() + 5));

    Buffer no;
    no.append("abc");
    EXPECT_EQ(nullptr, no.findCRLF());
    EXPECT_EQ(nullptr, no.findEOL());
}

// ---------------------------------------------------------------------------
// ★★ readFd —— 整个 Buffer 最关键的函数 ★★
// ---------------------------------------------------------------------------
namespace
{

int makeNonblockingPair(int fds[2])
{
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds);
}

}  // namespace

TEST(BufferTest, ReadFdBasic)
{
    int fds[2];
    ASSERT_EQ(0, makeNonblockingPair(fds));

    ASSERT_EQ(5, ::write(fds[0], "hello", 5));

    Buffer b;
    int savedErrno = 0;
    ssize_t n = b.readFd(fds[1], &savedErrno);

    EXPECT_EQ(5, n);
    EXPECT_EQ("hello", std::string(b.peek(), b.readableBytes()));

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, ReadFdUsesExtrabuf)
{
    int fds[2];
    ASSERT_EQ(0, makeNonblockingPair(fds));

    // 初始可写区只有 16 字节。一次写 5000 字节：
    // 前 16 字节进 Buffer，剩下的必须走栈上 extrabuf，
    // 再通过 append 追加进来 —— 这条分支不测就等于没测 readv。
    std::string payload(5000, 'z');
    ssize_t wrote = ::write(fds[0], payload.data(), payload.size());
    ASSERT_GT(wrote, 16);

    Buffer b(16);
    int savedErrno = 0;
    ssize_t n = b.readFd(fds[1], &savedErrno);

    ASSERT_EQ(wrote, n);
    EXPECT_EQ(static_cast<size_t>(wrote), b.readableBytes());
    EXPECT_EQ(payload.substr(0, static_cast<size_t>(wrote)),
              std::string(b.peek(), b.readableBytes()));

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, ReadFdEAGAIN)
{
    int fds[2];
    ASSERT_EQ(0, makeNonblockingPair(fds));

    // 对端没发数据：非阻塞 readv 返回 -1/EAGAIN，
    // Buffer 不能被破坏，errno 要原样交回给调用方
    Buffer b;
    int savedErrno = 0;
    ssize_t n = b.readFd(fds[1], &savedErrno);

    EXPECT_EQ(-1, n);
    EXPECT_EQ(EAGAIN, savedErrno);
    EXPECT_EQ(0u, b.readableBytes());

    ::close(fds[0]);
    ::close(fds[1]);
}
