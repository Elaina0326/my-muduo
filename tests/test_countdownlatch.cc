// ============================================================================
// test_countdownlatch.cc —— CountDownLatch 单元测试
// ============================================================================

#include "muduo/base/Atomic.h"
#include "muduo/base/CountDownLatch.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Timestamp.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>


// ---------------------------------------------------------------------------
// 用例 1：初始计数正确
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, InitialCount) {
    muduo::CountDownLatch latch(3);
    EXPECT_EQ(3, latch.getCount());
}

TEST(CountDownLatchTest, CountDownDecrementsOneByOne) {
    muduo::CountDownLatch latch(3);

    latch.countDown();
    EXPECT_EQ(2, latch.getCount());

    latch.countDown();
    EXPECT_EQ(1, latch.getCount());

    latch.countDown();
    EXPECT_EQ(0, latch.getCount());
}


// ---------------------------------------------------------------------------
// 用例 2：计数归零后 wait() 立即返回
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, WaitReturnsImmediatelyWhenAlreadyZero) {
    muduo::CountDownLatch latch(1);
    latch.countDown();

    const int64_t start = muduo::Timestamp::now().microSecondsSinceEpoch();
    latch.wait();
    const int64_t elapsed = muduo::Timestamp::now().microSecondsSinceEpoch() - start;

    // 已经归零，不该阻塞。给个宽松的阈值避免机器抖动误判。
    EXPECT_LT(elapsed, 50000);      // < 50ms
}


// ---------------------------------------------------------------------------
// 用例 3：计数归零【之前】，wait() 必须阻塞
//
// ★ 这条验的是"真的会等"，而不是"碰巧没卡住"。
//   用一个子线程延迟 countDown，主线程计时。
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, WaitBlocksUntilZero) {
    muduo::CountDownLatch latch(1);

    std::thread worker([&latch] {
        muduo::CurrentThread::sleepUsec(50000);    // 50ms 后才放行
        latch.countDown();
    });

    const int64_t start = muduo::Timestamp::now().microSecondsSinceEpoch();
    latch.wait();
    const int64_t elapsed = muduo::Timestamp::now().microSecondsSinceEpoch() - start;

    worker.join();

    EXPECT_EQ(0, latch.getCount());
    // 必须真的等了约 50ms。如果实现里 wait() 是空函数，这里会是 0。
    EXPECT_GE(elapsed, 50000);
}


// ---------------------------------------------------------------------------
// 用例 4：多个等待者都会被唤醒
//
// ★ 这条抓的是 countDown() 里写 notify() 而不是 notifyAll() 的 bug。
//   3 个线程同时 wait，计数归零时如果只叫醒一个，另外两个会永久卡死。
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, AllWaitersAreReleased) {
    constexpr int kWaiters = 3;
    muduo::CountDownLatch latch(kWaiters);

    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);

    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&latch] {
            latch.wait();      // 全部堵在这里
        });
    }

    // 等它们确实都进入 wait 了再放行（这一步不是必须的，
    // 因为 latch 的计数本身就保证了正确性，加了只是让场景更明确）
    muduo::CurrentThread::sleepUsec(30000);

    for (int i = 0; i < kWaiters; ++i) {
        latch.countDown();     // 减到 0 的那一刻会 notifyAll
    }

    // 只要有一个没被唤醒，这里就会卡到 ctest 的 60 秒超时
    for (auto& t : waiters) {
        t.join();
    }

    EXPECT_EQ(0, latch.getCount());
}


// ---------------------------------------------------------------------------
// 用例 5：典型用法 —— 等待 N 个线程各自完成一项工作
//
// 这就是 CountDownLatch 最经典的场景，也是 Day 5 Thread 要用的模式。
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, WaitsForAllWorkersToFinish) {
    constexpr int kWorkers = 4;
    muduo::CountDownLatch latch(kWorkers);

    // ★ 多线程共享的累加变量必须用【原子类型】。
    //   这里顺便用上了 Day 3 写的 Atomic.h。
    muduo::AtomicInt32 done;

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&] {
            muduo::CurrentThread::sleepUsec(20000);   // 模拟干活
            done.increment();
            latch.countDown();                        // ★ 干完就减一
        });
    }

    latch.wait();     // 主线程在这里等所有人干完

    // ★ 为什么这里可以直接断言 done == 4，而不需要 join？
    //
    //   CountDownLatch 内部那把锁建立了 happens-before 关系：
    //     · 每个 worker：done.increment() 在前，countDown() 在后
    //     · countDown() 持锁改写 count_，解锁时把之前的内存写"发布"出去
    //     · wait() 拿到同一把锁，看到 count_ == 0 时返回
    //     · 最后执行的那个 countDown 必然在所有 countDown 之后，
    //       而计数归零又要求 4 个都执行过
    //   所以 4 次 increment 的结果对主线程全部可见 —— done 一定等于 4。
    //
    //   这就是"锁不仅保护数据，还传递可见性"。
    EXPECT_EQ(0, latch.getCount());
    EXPECT_EQ(kWorkers, done.get());

    for (auto& t : workers) {
        t.join();
    }
}
