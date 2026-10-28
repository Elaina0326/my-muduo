// test_atomic.cc —— Atomic 单元测试
//
// 这个文件的写法就是 gtest 的标准结构：
//   ① 包含被测头文件 + <gtest/gtest.h>
//   ② 用 TEST(套件名, 用例名) { ... } 一个用例一个函数
//   ③ 里面用 EXPECT_EQ 之类的宏做断言
//   没有 main()——gtest_main 提供了。

#include "muduo/base/Atomic.h"

#include <gtest/gtest.h>

#include <thread>   // 最后一个用例要开线程

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

TEST(AtomicTest, DefaultIsZero) {
    muduo::AtomicInt32 n;
    EXPECT_EQ(0, n.get());
}

TEST(AtomicTest, ExplicitConstruct) {
    muduo::AtomicInt32 n(42);
    EXPECT_EQ(42, n.get());
}

// ---------------------------------------------------------------------------
// ★ 这两个方法的名字就说明了语义，测试要把它钉死
//
//   getAndAdd(x)  → 返回【加之前】的旧值
//   addAndGet(x)  → 返回【加之后】的新值
//
//   混了的话逻辑不会崩，但会静默算错 —— 正是该用测试钉死的地方。
// ---------------------------------------------------------------------------

TEST(AtomicTest, GetAndAddReturnsOldValue) {
    muduo::AtomicInt32 n(10);
    EXPECT_EQ(10, n.getAndAdd(5));   // 返回旧的 10
    EXPECT_EQ(15, n.get());          // 但内部已经变成 15
}

TEST(AtomicTest, AddAndGetReturnsNewValue) {
    muduo::AtomicInt32 n(10);
    EXPECT_EQ(15, n.addAndGet(5));   // 返回新的 15
    EXPECT_EQ(15, n.get());
}

// ---------------------------------------------------------------------------
// 自增 / 自减 —— 名字里带 AndGet，就该返回【新值】
// ---------------------------------------------------------------------------

TEST(AtomicTest, IncrementAndGetReturnsNewValue) {
    muduo::AtomicInt32 n(0);
    EXPECT_EQ(1, n.incrementAndGet());
    EXPECT_EQ(1, n.get());
}

TEST(AtomicTest, DecrementAndGetReturnsNewValue) {
    muduo::AtomicInt32 n(0);
    EXPECT_EQ(-1, n.decrementAndGet());
    EXPECT_EQ(-1, n.get());
}

// ---------------------------------------------------------------------------
// getAndSet —— 返回旧值
// ---------------------------------------------------------------------------

TEST(AtomicTest, GetAndSetReturnsOldValue) {
    muduo::AtomicInt32 n(7);
    EXPECT_EQ(7, n.getAndSet(99));
    EXPECT_EQ(99, n.get());
}

// ---------------------------------------------------------------------------
// 不关心返回值的版本
// ---------------------------------------------------------------------------

TEST(AtomicTest, VoidVersionsChangeValue) {
    muduo::AtomicInt32 n;
    n.increment();
    n.increment();
    EXPECT_EQ(2, n.get());

    n.add(10);
    EXPECT_EQ(12, n.get());

    n.set(-5);
    EXPECT_EQ(-5, n.get());
}

// ---------------------------------------------------------------------------
// ★ 并发自增 —— Atomic 存在的【唯一理由】
//
//   如果底层不是原子的，多个线程同时读-改-写会丢更新，
//   最终结果会小于 400000。这条测试就是为了证明它没丢。
// ---------------------------------------------------------------------------

TEST(AtomicTest, ConcurrentIncrementLosesNothing) {
    muduo::AtomicInt32 n;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100000;

    std::thread threads[kThreads];
    for (int i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&n] {
            for (int j = 0; j < kPerThread; ++j) {
                n.increment();
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(kThreads * kPerThread, n.get());   // 正好 400000，少一个都不行
}
