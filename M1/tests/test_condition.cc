// ============================================================================
// test_condition.cc —— Condition（条件变量）单元测试
//
// 语法部分如果看不懂，回头翻 test_mutex.cc 顶部的教学注释。
// 这里只标注本文件特有的东西。
// ============================================================================

#include "muduo/base/Condition.h"
#include "muduo/base/Mutex.h"
#include "muduo/base/Timestamp.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>


// ============================================================================
// 【测试脚手架】一个锁 + 一个条件变量 + 一个标志位
//
// Condition 不能单独用 —— 它必须配一把锁和一个"被保护的条件"。
// 所以每个测试都要先搭这么一个结构，写在匿名 namespace 里避免和别的文件撞名。
//
// 匿名 namespace = 里面的东西只在本文件可见，相当于给测试文件加了私有区。
// ============================================================================

namespace {

struct Signal {
    // ⚠️ 成员初始化顺序 = 【声明顺序】，不是初始化列表的书写顺序！
    //
    //    这里 mutex 声明在 cond 前面，所以构造函数里 cond_(mutex) 是合法的
    //    —— 进 cond 的构造函数时 mutex 已经构造完了。
    //
    //    如果把 cond 声明在 mutex 之前，即使初始化列表写成 cond_(mutex)，
    //    编译时也会先构造 cond，此时 mutex 还是空的 → 未定义行为。
    //    编译器对这种顺序不一致会警告（-Wreorder），别忽略它。
    muduo::MutexLock mutex;
    muduo::Condition cond;
    bool ready = false;          // 被保护的条件

    Signal() : mutex(), cond(mutex), ready(false) {}
};

}  // namespace


// ---------------------------------------------------------------------------
// 用例 1：wait() 被 notify() 唤醒
// ---------------------------------------------------------------------------

TEST(ConditionTest, WaitWakesAfterNotify) {
    Signal sig;

    // 子线程：等 ready 变真
    std::thread waiter([&sig] {
        muduo::MutexLockGuard lock(sig.mutex);

        // ★ 一定是 while，不能是 if
        //   1. 虚假唤醒：POSIX 允许 wait 在条件不满足时也返回
        //   2. 被唤醒到重新拿到锁之间有窗口，条件可能又被改回去了
        while (!sig.ready) {
            sig.cond.wait();
        }
        // wait 返回时就重新持有锁了
    });

    // 主线程：稍等一下，设标志，然后唤醒
    muduo::CurrentThread::sleepUsec(10000);   // 10 毫秒

    {
        muduo::MutexLockGuard lock(sig.mutex);
        sig.ready = true;
        sig.cond.notify();
    }

    waiter.join();     // 如果没有被唤醒，这里会一直卡住 → 用例超时失败
}


// ---------------------------------------------------------------------------
// 用例 2：waitForSeconds 超时返回 true
// ---------------------------------------------------------------------------

TEST(ConditionTest, WaitForSecondsTimesOut) {
    Signal sig;

    muduo::MutexLockGuard lock(sig.mutex);

    const int64_t start = muduo::Timestamp::now().microSecondsSinceEpoch();
    const bool timedOut = sig.cond.waitForSeconds(0.05);      // 等 50ms
    const int64_t elapsed = muduo::Timestamp::now().microSecondsSinceEpoch() - start;

    EXPECT_TRUE(timedOut);                    // 没人 notify → 应该超时
    EXPECT_GE(elapsed, 50000);                // 至少等够了 50ms
}


// ---------------------------------------------------------------------------
// 用例 3：waitForSeconds 被提前唤醒返回 false
//
// 这条比用例 2 更有价值：它同时验了"能醒"和"返回值和超时区分得开"。
// ---------------------------------------------------------------------------

TEST(ConditionTest, WaitForSecondsWakesBeforeTimeout) {
    Signal sig;

    std::thread notifier([&sig] {
        muduo::CurrentThread::sleepUsec(20000);      // 20ms 后唤醒
        muduo::MutexLockGuard lock(sig.mutex);
        sig.ready = true;
        sig.cond.notify();
    });

    const int64_t start = muduo::Timestamp::now().microSecondsSinceEpoch();

    // ⚠️ MutexLockGuard 【没有】 unlock() 方法 —— 它只能在析构时解锁。
    //    所以想手动控制解锁时机，就得用 [ 大括号 ] 划出作用域。
    //    这本身就是 RAII 的设计意图：锁的生命周期 = 变量的生命周期。
    bool timedOut = true;
    {
        muduo::MutexLockGuard lock(sig.mutex);
        while (!sig.ready) {
            // 超时给了 1 秒，但 20ms 后就会被唤醒 → 返回 false
            timedOut = sig.cond.waitForSeconds(1.0);
        }
    }   // ← 这里才解锁

    const int64_t elapsed = muduo::Timestamp::now().microSecondsSinceEpoch() - start;
    notifier.join();

    EXPECT_FALSE(timedOut);          // 不是超时醒的，是被 notify 唤醒的
    EXPECT_TRUE(sig.ready);
    EXPECT_LT(elapsed, 500000);      // 实际耗时 < 500ms，远小于 1 秒 → 确实提前醒了
}


// ---------------------------------------------------------------------------
// 用例 4：notifyAll 能唤醒【全部】等待者
//
// ★ 这条是专门验 notify vs notifyAll 的。
//   如果实现里写的是 notify()（只叫醒一个），这里会有线程永远卡住。
// ---------------------------------------------------------------------------

TEST(ConditionTest, NotifyAllWakesEveryone) {
    Signal sig;

    constexpr int kWaiters = 5;
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);

    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&sig] {
            muduo::MutexLockGuard lock(sig.mutex);
            while (!sig.ready) {
                sig.cond.wait();
            }
        });
    }

    muduo::CurrentThread::sleepUsec(20000);      // 给它们时间都进入 wait

    {
        muduo::MutexLockGuard lock(sig.mutex);
        sig.ready = true;
        sig.cond.notifyAll();        // ← 必须是 All
    }

    // 5 个线程都必须能退出。用 notify() 的话这里会卡死到超时。
    for (auto& t : waiters) {
        t.join();
    }

    SUCCEED();   // 走到这里就说明全都被唤醒了
}
