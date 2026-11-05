// test_blockingqueue.cc —— BlockingQueue 单元测试
//
// 你的实现是【无界】队列，take() 返回 bool：
//   队列空且未关闭  → 阻塞
//   队列空且已关闭  → 返回 false（消费者据此退出）
//   队列非空        → 取出元素返回 true

#include "muduo/base/Atomic.h"
#include "muduo/base/BlockingQueue.h"
#include "muduo/base/CurrentThread.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>


// ---------------------------------------------------------------------------
// 基本存取
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, PutThenTake) {
    muduo::BlockingQueue<int> q;

    q.put(1);
    q.put(2);
    q.put(3);
    EXPECT_EQ(3u, q.size());

    int v = 0;
    EXPECT_TRUE(q.take(&v));
    EXPECT_EQ(1, v);
    EXPECT_TRUE(q.take(&v));
    EXPECT_EQ(2, v);
    EXPECT_TRUE(q.take(&v));
    EXPECT_EQ(3, v);

    EXPECT_EQ(0u, q.size());
}


TEST(BlockingQueueTest, PreservesFifoOrder) {
    muduo::BlockingQueue<int> q;
    for (int i = 0; i < 100; ++i) {
        q.put(i * 7);
    }

    int v = 0;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(q.take(&v));
        EXPECT_EQ(i * 7, v);
    }
}


// 移动版本：传右值应该走 put(T&&)，避免拷贝
TEST(BlockingQueueTest, MoveVersionDoesNotCopy) {
    muduo::BlockingQueue<std::string> q;

    std::string s = "hello";
    q.put(s);                       // 左值 → 拷贝
    q.put(std::string("world"));    // 右值 → 移动

    int v = 0;
    (void)v;
    std::string out;
    EXPECT_TRUE(q.take(&out));
    EXPECT_EQ("hello", out);
    EXPECT_TRUE(q.take(&out));
    EXPECT_EQ("world", out);

    EXPECT_EQ("hello", s);          // 左值那次是拷贝，s 不受影响
}


// ---------------------------------------------------------------------------
// 阻塞语义
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, TakeBlocksWhileEmpty) {
    muduo::BlockingQueue<int> q;
    muduo::AtomicInt32 got;

    std::thread consumer([&] {
        int v = 0;
        q.take(&v);                 // 队列空 → 一直阻塞
        got.set(v);
    });

    muduo::CurrentThread::sleepUsec(50000);   // 50ms
    EXPECT_EQ(0, got.get());                  // 还没拿到东西

    q.put(42);                                // 放行
    consumer.join();
    EXPECT_EQ(42, got.get());
}


TEST(BlockingQueueTest, MultipleElementsWakeOneConsumerAtATime) {
    muduo::BlockingQueue<int> q;
    muduo::AtomicInt32 count;

    std::thread c1([&] {
        int v = 0;
        while (q.take(&v)) { count.increment(); }
    });
    std::thread c2([&] {
        int v = 0;
        while (q.take(&v)) { count.increment(); }
    });

    for (int i = 0; i < 100; ++i) {
        q.put(i);
    }
    // 等两个消费者把队列取空
    while (q.size() > 0) {
        muduo::CurrentThread::sleepUsec(1000);
    }
    muduo::CurrentThread::sleepUsec(20000);

    q.close();                     // 让两个消费者退出
    c1.join();
    c2.join();

    EXPECT_EQ(100, count.get());   // 一个不多一个不少
}


// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, CloseMakesTakeReturnFalse) {
    muduo::BlockingQueue<int> q;
    q.close();

    int v = 0;
    EXPECT_FALSE(q.take(&v));       // 已关闭且空 → false
}


// ★ 关键：close 不是"丢弃"，而是"取完剩下的再退出"
TEST(BlockingQueueTest, CloseStillDrainsRemaining) {
    muduo::BlockingQueue<int> q;
    q.put(1);
    q.put(2);
    q.put(3);
    q.close();

    std::vector<int> got;
    int v = 0;
    while (q.take(&v)) {
        got.push_back(v);
    }

    ASSERT_EQ(3u, got.size());      // 关闭前放进去的 3 个一个不丢
    EXPECT_EQ(1, got[0]);
    EXPECT_EQ(2, got[1]);
    EXPECT_EQ(3, got[2]);
}


// ★ close() 能把【正在阻塞】的消费者唤醒 —— 这是它存在的核心理由。
//   没有它，生产者不再 put 时消费者会永久挂死。
TEST(BlockingQueueTest, CloseWakesBlockedConsumer) {
    muduo::BlockingQueue<int> q;
    muduo::AtomicInt32 returnedFalse;

    std::thread consumer([&] {
        int v = 0;
        if (!q.take(&v)) {          // 阻塞中
            returnedFalse.set(1);
        }
    });

    muduo::CurrentThread::sleepUsec(50000);
    EXPECT_EQ(0, returnedFalse.get());   // 还在阻塞

    q.close();                            // 唤醒它
    consumer.join();                      // 没唤醒的话这里会卡到 ctest 超时
    EXPECT_EQ(1, returnedFalse.get());
}


TEST(BlockingQueueTest, MultipleWaitersAllWokenByClose) {
    constexpr int kWaiters = 5;
    muduo::BlockingQueue<int> q;
    muduo::AtomicInt32 exits;

    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&] {
            int v = 0;
            while (q.take(&v)) { /* 阻塞 */ }
            exits.increment();
        });
    }

    muduo::CurrentThread::sleepUsec(50000);
    q.close();                    // notifyAll 才能唤醒全部

    for (auto& t : waiters) {
        t.join();                 // 漏掉一个就会卡到超时
    }
    EXPECT_EQ(kWaiters, exits.get());
}


// ---------------------------------------------------------------------------
// drain() / size()
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, DrainTakesEverything) {
    muduo::BlockingQueue<int> q;
    for (int i = 0; i < 50; ++i) {
        q.put(i);
    }

    auto all = q.drain();
    EXPECT_EQ(50u, all.size());
    EXPECT_EQ(0u, q.size());
    EXPECT_EQ(0, all.front());
    EXPECT_EQ(49, all.back());
}


// ---------------------------------------------------------------------------
// 多生产者多消费者
// ---------------------------------------------------------------------------

TEST(BlockingQueueTest, MultiProducerMultiConsumerLosesNothing) {
    muduo::BlockingQueue<int> q;

    constexpr int kProducers = 2;
    constexpr int kPerProducer = 500;
    constexpr int kConsumers = 3;
    constexpr int kTotal = kProducers * kPerProducer;

    muduo::AtomicInt32 count;
    muduo::AtomicInt64 sum;

    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            int v = 0;
            while (q.take(&v)) {
                sum.addAndGet(v);
                count.increment();
            }
        });
    }

    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        // ★ 按值捕获 i，不能按引用（循环变量会变）
        producers.emplace_back([&q, i] {
            for (int j = 0; j < kPerProducer; ++j) {
                q.put(i * kPerProducer + j);     // 合起来正好 0..999
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }
    q.close();                      // 生产者都结束了，通知消费者退出

    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(kTotal, count.get());       // 一个不多一个不少
    EXPECT_EQ(999 * 1000 / 2, sum.get()); // 内容没重复没丢失
}
