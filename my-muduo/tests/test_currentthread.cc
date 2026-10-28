// test_currentthread.cc —— CurrentThread 单元测试

#include "muduo/base/CurrentThread.h"
#include "muduo/base/Timestamp.h"    // 最后一个用例要用 now() 计时

#include <gtest/gtest.h>

#include <sys/syscall.h>   // SYS_gettid
#include <unistd.h>        // getpid

#include <thread>

// ---------------------------------------------------------------------------
// tid() —— 必须和内核的 TID 一致
//
// 注意不是 pthread_self()。pthread_self() 返回的是线程库内部句柄，
// 和 top -H 里看到的那个号不是一回事。
// ---------------------------------------------------------------------------

TEST(CurrentThreadTest, TidMatchesKernelTid) {
    const int tid = muduo::CurrentThread::tid();
    EXPECT_GT(tid, 0);
    EXPECT_EQ(static_cast<int>(::syscall(SYS_gettid)), tid);
}

// ---------------------------------------------------------------------------
// 缓存 —— 多次调用结果必须一致，且 tidString 已就绪
// ---------------------------------------------------------------------------

TEST(CurrentThreadTest, TidStringIsReadyAfterTidCall) {
    muduo::CurrentThread::tid();          // 触发一次缓存
    EXPECT_GT(muduo::CurrentThread::tidStringLength(), 0);
    EXPECT_STRNE("", muduo::CurrentThread::tidString());
    EXPECT_STREQ("unknown", muduo::CurrentThread::name());
}

// ---------------------------------------------------------------------------
// isMainThread —— 主线程为真
// ---------------------------------------------------------------------------

TEST(CurrentThreadTest, MainThreadIsMain) {
    EXPECT_TRUE(muduo::CurrentThread::isMainThread());
}

// ---------------------------------------------------------------------------
// 子线程 —— tid 不同，且 isMainThread 为假
// ---------------------------------------------------------------------------

TEST(CurrentThreadTest, ChildThreadHasOwnTid) {
    const int mainTid = muduo::CurrentThread::tid();

    int childTid = 0;
    bool childIsMain = true;

    std::thread t([&childTid, &childIsMain] {
        childTid = muduo::CurrentThread::tid();
        childIsMain = muduo::CurrentThread::isMainThread();
    });
    t.join();

    EXPECT_NE(mainTid, childTid);        // 每个线程有自己的 tid
    EXPECT_FALSE(childIsMain);           // 子线程不是主线程
}

// ---------------------------------------------------------------------------
// sleepUsec —— 至少睡够时间
// ---------------------------------------------------------------------------

TEST(CurrentThreadTest, SleepUsec) {
    const int64_t start = muduo::Timestamp::now().microSecondsSinceEpoch();
    muduo::CurrentThread::sleepUsec(10000);      // 睡 10 毫秒
    const int64_t end = muduo::Timestamp::now().microSecondsSinceEpoch();

    EXPECT_GE(end - start, 10000);               // 不能早醒
}
