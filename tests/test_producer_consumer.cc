// ============================================================================
// test_producer_consumer.cc —— 生产者-消费者
//
// BlockingQueue 是 Day 6 的内容，所以这里【手搓】一个极简队列。
// 手搓的价值：你亲手体会一遍"锁 + 条件变量"怎么配合，
// 到了 Day 6 看 BlockingQueue 就只是"把这段搬进模板"而已。
//
// ★ 这个文件最该跑一遍 TSan：
//     cmake -S . -B build-tsan -DENABLE_TSAN=ON
//     cmake --build build-tsan -j4
//     ctest --test-dir build-tsan --output-on-failure
//   逻辑正确 ≠ 没有数据竞争。
// ============================================================================

#include "muduo/base/Atomic.h"
#include "muduo/base/Condition.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Mutex.h"

#include <gtest/gtest.h>

#include <queue>
#include <thread>
#include <vector>


namespace {

// ===========================================================================
// SimpleQueue —— 一个有界阻塞队列的最小实现
//
// 三个操作：
//   put(v)  —— 放一个元素，然后唤醒一个等着的消费者
//   take()  —— 取一个元素，空了就阻塞等待
//   size()  —— 看一眼当前长度
// ===========================================================================
class SimpleQueue {
public:
    // ⚠️ 必须显式写这个构造函数。
    //
    //   Condition 的构造函数是 explicit Condition(MutexLock&)，
    //   【没有默认构造函数】—— 因为条件变量必须绑定一把锁才有意义。
    //   所以 SimpleQueue 必须自己把 mutex_ 传给 cond_。
    //
    //   另外：成员初始化顺序 = 声明顺序（mutex_ 在前），
    //   所以初始化列表里 cond_(mutex_) 是安全的。
    SimpleQueue() : mutex_(), cond_(mutex_) {}

    void put(int value) {
        muduo::MutexLockGuard lock(mutex_);
        queue_.push(value);
        // notify 只叫醒一个等待者。每个 put 对应一个元素，
        // 所以叫醒一个就够了（叫醒更多也没用，它们会重新 wait）。
        cond_.notify();
    }

    int take() {
        muduo::MutexLockGuard lock(mutex_);
        // ★ 必须是 while：醒来后队列可能又被别的消费者抢空了
        while (queue_.empty()) {
            cond_.wait();
        }
        const int value = queue_.front();
        queue_.pop();
        return value;
    }

    size_t size() const {
        // mutable 的意义：让 const 成员函数也能改 mutex_ 的内部状态
        muduo::MutexLockGuard lock(mutex_);
        return queue_.size();
    }

private:
    mutable muduo::MutexLock mutex_;
    muduo::Condition cond_;
    std::queue<int> queue_;
};

// 0..999 的和，用来一次性校验"一个不多一个不少且顺序无关"
constexpr int64_t kSum0To999 = 999 * 1000 / 2;   // 499500

}  // namespace


// ---------------------------------------------------------------------------
// 用例 1：先放后取，内容与总和都对
// ---------------------------------------------------------------------------

TEST(SimpleQueueTest, PutThenTake) {
    SimpleQueue q;

    for (int i = 0; i < 1000; ++i) {
        q.put(i);
    }
    EXPECT_EQ(1000u, q.size());

    int64_t sum = 0;
    for (int i = 0; i < 1000; ++i) {
        sum += q.take();
    }

    EXPECT_EQ(kSum0To999, sum);
    EXPECT_EQ(0u, q.size());
}


// ---------------------------------------------------------------------------
// 用例 2：先进先出顺序正确
// ---------------------------------------------------------------------------

TEST(SimpleQueueTest, PreservesFifoOrder) {
    SimpleQueue q;

    for (int i = 0; i < 10; ++i) {
        q.put(i * 10);
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(i * 10, q.take());     // EXPECT_EQ 在循环里用也没问题
    }
}


// ---------------------------------------------------------------------------
// 用例 3：队列为空时 take() 必须阻塞
//
// ★ 验"真的会等"，而不是"碰巧没卡"。
// ---------------------------------------------------------------------------

TEST(SimpleQueueTest, TakeBlocksWhileEmpty) {
    SimpleQueue q;

    muduo::AtomicInt32 taken;      // 初始 0

    std::thread consumer([&q, &taken] {
        // 队列是空的，这里会一直堵着
        taken.set(q.take());
    });

    muduo::CurrentThread::sleepUsec(30000);      // 30ms

    // 这段时间里消费者应该没拿到任何东西
    EXPECT_EQ(0, taken.get());

    q.put(42);                                    // 放行
    consumer.join();

    EXPECT_EQ(42, taken.get());
}


// ---------------------------------------------------------------------------
// 用例 4：多生产者 + 多消费者 —— 一个不多，一个不少
//
// 这是整个文件最有价值的一条。
// 2 个生产者各放 500 个，3 个消费者一起取，
// 最后校验：取到的个数 = 1000，取到的总和 = 499500。
// 少一个或重复一个都会被抓出来。
// ---------------------------------------------------------------------------

TEST(SimpleQueueTest, MultiProducerMultiConsumer) {
    SimpleQueue q;

    constexpr int kProducers = 2;
    constexpr int kPerProducer = 500;
    constexpr int kConsumers = 3;
    constexpr int kTotal = kProducers * kPerProducer;      // 1000

    // 哨兵值：消费者取到它就退出。
    // 元素范围是 0..999，所以 -1 不会和真实数据冲突。
    constexpr int kSentinel = -1;

    muduo::AtomicInt32 countConsumed;
    muduo::AtomicInt64 sumConsumed;

    // ---- 消费者：一直取，直到拿到哨兵 ----
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            while (true) {
                const int v = q.take();
                if (v == kSentinel) {
                    break;
                }
                sumConsumed.addAndGet(v);
                countConsumed.increment();
            }
        });
    }

    // ---- 生产者：各放 500 个 ----
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int i = 0; i < kProducers; ++i) {
        // ★ 这里必须【按值】捕获 i（写成 [&, i]），不能按引用。
        //   按引用捕获的话，循环变量 i 在生产者线程真正跑起来时
        //   可能已经变成别的值了 —— 经典的悬垂引用 bug。
        producers.emplace_back([&q, i] {
            for (int j = 0; j < kPerProducer; ++j) {
                q.put(i * kPerProducer + j);      // 合起来正好是 0..999
            }
        });
    }

    for (auto& t : producers) {
        t.join();          // 等生产完
    }

    // ---- 放哨兵，让每个消费者都能退出 ----
    for (int i = 0; i < kConsumers; ++i) {
        q.put(kSentinel);
    }

    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(kTotal, countConsumed.get());       // 一个不多一个不少
    EXPECT_EQ(kSum0To999, sumConsumed.get());     // 内容也没重复没丢失
}


// ---------------------------------------------------------------------------
// 用例 5：积压之后消费者能把队列清空
//
// 让生产者先跑完（队列里堆 500 个），消费者再开始 —— 验"积压能取出来"。
// ---------------------------------------------------------------------------

TEST(SimpleQueueTest, DrainsBacklog) {
    SimpleQueue q;

    for (int i = 0; i < 500; ++i) {
        q.put(i);
    }
    EXPECT_EQ(500u, q.size());

    int64_t sum = 0;
    for (int i = 0; i < 500; ++i) {
        sum += q.take();
    }

    EXPECT_EQ(499 * 500 / 2, sum);
    EXPECT_EQ(0u, q.size());
}
