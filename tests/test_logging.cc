// test_logging.cc —— Logging 单元测试
//
// 难点说明：
//
//   ① Logger::OutputFunc 是【裸函数指针】（不是 std::function），
//      不能捕获状态 —— 所以「捕获输出」只能用一个全局缓冲区。
//
//   ② LOG_FATAL 会 abort()，直接测会把测试进程一起带走，
//      所以必须在【子进程】里验（fork + waitpid 检查 SIGABRT）。

#include "muduo/base/Logging.h"
#include "muduo/base/CurrentThread.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 全局捕获缓冲 —— 因为 OutputFunc 是裸函数指针，没法用 lambda 捕获
// ---------------------------------------------------------------------------
std::mutex g_mutex;
std::string g_captured;

void captureOutput(const char* msg, int len) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_captured.append(msg, static_cast<size_t>(len));
}

std::string takeCaptured() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    out.swap(g_captured);
    return out;
}

void clearCaptured() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_captured.clear();
}

// 把输出接到捕获函数、级别拉到 TRACE —— 每个用例开头都要做
void armCapture() {
    clearCaptured();
    muduo::Logger::setLogLevel(muduo::TRACE);
    muduo::Logger::setOutput(captureOutput);
}

// 时间戳格式：<秒>.<6 位微秒>
bool startsWithTimestamp(const std::string& line) {
    static const std::regex re(R"(^\d+\.\d{6} )");
    return std::regex_search(line, re);
}

// 去掉结尾换行，按行拆
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

}  // namespace


// ===========================================================================
// 输出格式
// ===========================================================================

TEST(LoggingTest, FormatHasTimestampTidLevelAndLocation) {
    armCapture();

    LOG_INFO << "hello";

    std::string out = takeCaptured();
    ASSERT_FALSE(out.empty());
    EXPECT_EQ('\n', out.back());              // 自动补换行

    std::string line = out.substr(0, out.size() - 1);
    EXPECT_TRUE(startsWithTimestamp(line)) << "实际: " << line;

    // 含 TID
    EXPECT_NE(std::string::npos,
              line.find(std::to_string(muduo::CurrentThread::tid())));
    // 含级别名
    EXPECT_NE(std::string::npos, line.find("INFO"));
    // 含 文件:行号]
    EXPECT_NE(std::string::npos, line.find("test_logging.cc:"));
    EXPECT_NE(std::string::npos, line.find("] "));
    // 含消息体
    EXPECT_NE(std::string::npos, line.find("hello"));
}


TEST(LoggingTest, StreamAcceptsMultipleTypes) {
    armCapture();

    LOG_INFO << "s=" << 42 << " d=" << 3.5 << " c=" << 'x';

    std::string out = takeCaptured();
    EXPECT_NE(std::string::npos, out.find("s=42"));
    EXPECT_NE(std::string::npos, out.find("d=3.5"));
    EXPECT_NE(std::string::npos, out.find("c=x"));
}


TEST(LoggingTest, StripsTrailingNewlinesFromMessage) {
    armCapture();

    LOG_INFO << "no-double-newline\n";     // 用户自己写了换行

    std::string out = takeCaptured();
    ASSERT_FALSE(out.empty());
    // 结尾只能有一个换行（多余的被吃掉）
    EXPECT_EQ('\n', out.back());
    EXPECT_NE(std::string("\n\n"), out.substr(out.size() >= 2 ? out.size() - 2 : 0));
    EXPECT_EQ(1u, splitLines(out).size());
}


TEST(LoggingTest, DifferentLevelsShowTheirNames) {
    armCapture();

    LOG_TRACE << "t";
    LOG_DEBUG << "d";
    LOG_INFO  << "i";
    LOG_WARN  << "w";
    LOG_ERROR << "e";

    std::string out = takeCaptured();
    EXPECT_NE(std::string::npos, out.find("TRACE"));
    EXPECT_NE(std::string::npos, out.find("DEBUG"));
    EXPECT_NE(std::string::npos, out.find("INFO"));
    EXPECT_NE(std::string::npos, out.find("WARN"));
    EXPECT_NE(std::string::npos, out.find("ERROR"));
}


// ===========================================================================
// 级别过滤
// ===========================================================================

TEST(LoggingTest, LowerLevelsAreFiltered) {
    armCapture();
    muduo::Logger::setLogLevel(muduo::WARN);

    LOG_TRACE << "TRACE_GONE";
    LOG_DEBUG << "DEBUG_GONE";
    LOG_INFO  << "INFO_GONE";
    LOG_WARN  << "WARN_KEPT";

    std::string out = takeCaptured();
    EXPECT_EQ(std::string::npos, out.find("TRACE_GONE"));
    EXPECT_EQ(std::string::npos, out.find("DEBUG_GONE"));
    EXPECT_EQ(std::string::npos, out.find("INFO_GONE"));
    EXPECT_NE(std::string::npos, out.find("WARN_KEPT"));
}


TEST(LoggingTest, SetLogLevelIsObservable) {
    armCapture();

    muduo::Logger::setLogLevel(muduo::ERROR);
    EXPECT_EQ(muduo::ERROR, muduo::Logger::logLevel());

    muduo::Logger::setLogLevel(muduo::TRACE);
    EXPECT_EQ(muduo::TRACE, muduo::Logger::logLevel());
}


// ===========================================================================
// LOG_SYSERR 带 errno
// ===========================================================================

TEST(LoggingTest, SyserrAppendsErrnoDescription) {
    armCapture();

    errno = 0;
    int fd = ::open("/definitely/not/here/at/all", O_RDONLY);
    (void)fd;
    ASSERT_TRUE(errno != 0) << "本轮没能制造出 errno，测试前提不成立";

    LOG_SYSERR << "open failed";

    std::string out = takeCaptured();
    EXPECT_NE(std::string::npos, out.find("open failed"));
    EXPECT_NE(std::string::npos, out.find("errno="));           // 带 errno 数值
    EXPECT_NE(std::string::npos, out.find(std::to_string(errno)));
    EXPECT_NE(std::string::npos, out.find("No such file"));     // strerror 文本
}


// ★ errno 为 0 时不该追加后缀 —— 否则每条 SYSERR 都拖着 ": Success (errno=0)"
TEST(LoggingTest, SyserrWithoutErrnoHasNoSuffix) {
    armCapture();

    errno = 0;
    LOG_SYSERR << "NOERR_MARKER";

    std::string out = takeCaptured();
    ASSERT_NE(std::string::npos, out.find("NOERR_MARKER"));
    EXPECT_EQ(std::string::npos, out.find("errno="));
}


// ★★ errno 必须在【构造时】快照，不能在析构时现读
//
//   这条用例就是钉死这一点：日志表达式中途调用了一个会改 errno 的东西，
//   最终打出来的仍应是构造那一刻的值。
TEST(LoggingTest, SyserrSnapshotsErrnoAtConstruction) {
    armCapture();

    errno = ENOENT;                     // 2

    LOG_SYSERR << "MARKER" << ([] {
        errno = EACCES;                 // 表达式里偷偷改掉
        return "";
    })();

    std::string out = takeCaptured();
    EXPECT_NE(std::string::npos, out.find("errno=2"))
        << "errno 被后续表达式污染了 —— 说明没有在构造时快照。实际输出: " << out;
    EXPECT_EQ(std::string::npos, out.find("errno=13"));
}


// ===========================================================================
// setOutput
// ===========================================================================

TEST(LoggingTest, CustomOutputReceivesFormattedLine) {
    armCapture();

    LOG_INFO << "CUSTOM_MARKER";

    std::string out = takeCaptured();
    EXPECT_NE(std::string::npos, out.find("CUSTOM_MARKER"));
    EXPECT_TRUE(startsWithTimestamp(out));      // 已经是格式化后的完整行
}


// ===========================================================================
// 多线程不撕行
// ===========================================================================

TEST(LoggingTest, ConcurrentLoggingDoesNotTearLines) {
    armCapture();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 60;

    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([i] {
            for (int j = 0; j < kPerThread; ++j) {
                LOG_INFO << "TH" << i << "_" << j;
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }

    std::string out = takeCaptured();
    auto lines = splitLines(out);

    ASSERT_EQ(static_cast<size_t>(kThreads * kPerThread), lines.size())
        << "行数不对 —— 说明有日志被撕开或丢失";

    // 每一行都必须是【完整的一行】：前缀合法 + 内容完整
    static const std::regex re(R"(^\d+\.\d{6} +\d+ (TRACE|DEBUG|INFO|WARN|ERROR|FATAL) +test_logging\.cc:\d+\] TH\d+_\d+$)");
    size_t bad = 0;
    for (const auto& l : lines) {
        if (!std::regex_match(l, re)) {
            ++bad;
            if (bad <= 3) {
                ADD_FAILURE() << "残缺行: [" << l << "]";
            }
        }
    }
    EXPECT_EQ(0u, bad);
}


// ===========================================================================
// LOG_FATAL 会 abort —— 必须在子进程里测
// ===========================================================================

TEST(LoggingTest, FatalAbortsTheProcess) {
    // 先把输出接到 /dev/null 一样的黑洞，避免子进程往测试输出里灌垃圾。
    // 注意不能设成 nullptr —— Logger 会直接调用它。
    static auto sink = [](const char*, int) {};
    muduo::Logger::setOutput(sink);

    pid_t pid = ::fork();
    ASSERT_NE(-1, pid) << "fork 失败";

    if (pid == 0) {
        // ---- 子进程 ----
        // 注意：这里【不】刷 std::cout 之类的缓冲，直接打日志再 abort
        LOG_FATAL << "FATAL_MARKER";
        ::_exit(0);      // 不该执行到这里 —— 执行到说明 FATAL 没终止进程
    }

    // ---- 父进程 ----
    int status = 0;
    ASSERT_EQ(pid, ::waitpid(pid, &status, 0));

    EXPECT_TRUE(WIFSIGNALED(status))
        << "子进程正常退出了 —— LOG_FATAL 没有终止进程"
        << "（exit code " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << "）";
    if (WIFSIGNALED(status)) {
        EXPECT_EQ(SIGABRT, WTERMSIG(status));
    }
}
