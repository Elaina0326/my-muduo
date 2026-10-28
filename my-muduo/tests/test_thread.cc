// test_thread.cc —— Thread 单元测试
//
// 核心考点：start() 返回时保证什么、不保证什么。

#include "muduo/base/Atomic.h"
#include "muduo/base/CountDownLatch.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Mutex.h"
#include "muduo/base/Thread.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>


// ---------------------------------------------------------------------------
// start() 的语义
// ---------------------------------------------------------------------------

TEST(ThreadTest, StartMakesTidReady) {
    muduo::Thread t([] {}, "Ready");
    EXPECT_FALSE(t.started());

    t.start();

    EXPECT_TRUE(t.started());
    EXPECT_GT(t.tid(), 0);          // ★ start() 保证 tid 已就绪
    t.join();
}


// ★ 这个文件里最重要的一条
//
// start() 只等"线程把自己的 tid 写出来"，
// 【不】等用户函数执行完。要等函数跑完必须 join()。
TEST(ThreadTest, StartDoesNotWaitForFuncToFinish) {
    muduo::CountDownLatch release(1);   // 用来卡住用户函数
    muduo::AtomicInt32 done;

    muduo::Thread t([&] {
        release.wait();                 // 堵在这里，直到主线程放行
        done.set(1);
    });
    t.start();

    // start() 已经返回，但函数被 release 卡着，绝不可能完成
    EXPECT_EQ(0, done.get());           // ★ 函数还没跑完
    EXPECT_GT(t.tid(), 0);              // ★ 但 tid 已经就绪

    release.countDown();                // 放行
    t.join();
    EXPECT_EQ(1, done.get());           // 现在才完成
}


TEST(ThreadTest, JoinWaitsForFunc) {
    muduo::AtomicInt32 done;
    muduo::Thread t([&done] { done.set(1); });

    t.start();
    t.join();

    EXPECT_EQ(1, done.get());           // join 返回 ⟹ 函数已执行完
}


// ---------------------------------------------------------------------------
// tid
// ---------------------------------------------------------------------------

TEST(ThreadTest, TidMatchesKernelTid) {
    pid_t inside = 0;
    muduo::Thread t([&inside] { inside = muduo::CurrentThread::tid(); });

    t.start();
    t.join();

    EXPECT_EQ(t.tid(), inside);         // Thread 缓存的 tid 就是线程自己的
    EXPECT_GT(t.tid(), 0);
}


TEST(ThreadTest, UniqueTidsAcrossManyThreads) {
    constexpr int kThreads = 100;

    std::set<pid_t> tids;
    std::vector<std::unique_ptr<muduo::Thread>> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(new muduo::Thread([] {}, "Many"));
        threads.back()->start();
        tids.insert(threads.back()->tid());     // start() 后立刻可取
    }
    for (auto& t : threads) {
        t->join();
    }

    EXPECT_EQ(static_cast<size_t>(kThreads), tids.size());   // 100 个线程，100 个不同 tid
}


// ---------------------------------------------------------------------------
// 命名
// ---------------------------------------------------------------------------

TEST(ThreadTest, ExplicitName) {
    muduo::Thread t([] {}, "MyWorker");
    EXPECT_EQ("MyWorker", t.name());
    t.start();
    t.join();
}

TEST(ThreadTest, DefaultNameIsGenerated) {
    muduo::Thread t([] {});
    EXPECT_FALSE(t.name().empty());
    EXPECT_NE(std::string::npos, t.name().find("Thread"));   // 形如 "ThreadN"
    t.start();
    t.join();
}

TEST(ThreadTest, NumCreatedIncrements) {
    const int before = muduo::Thread::numCreated();

    {
        muduo::Thread a([] {});
        muduo::Thread b([] {});
        EXPECT_EQ(before + 2, muduo::Thread::numCreated());
    }
}


// ---------------------------------------------------------------------------
// 并发跑：每个线程改自己的数据，互不干扰
// ---------------------------------------------------------------------------

TEST(ThreadTest, RunsFuncOnOwnThread) {
    constexpr int kThreads = 8;

    muduo::MutexLock mutex;
    std::vector<pid_t> seen;

    std::vector<std::unique_ptr<muduo::Thread>> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(new muduo::Thread([&mutex, &seen] {
            muduo::MutexLockGuard lock(mutex);      // vector 不是线程安全的
            seen.push_back(muduo::CurrentThread::tid());
        }));
    }
    for (auto& t : threads) {
        t->start();
    }
    for (auto& t : threads) {
        t->join();
    }

    EXPECT_EQ(static_cast<size_t>(kThreads), seen.size());
    for (pid_t tid : seen) {
        EXPECT_NE(muduo::CurrentThread::tid(), tid);   // 都不是主线程
    }
}
