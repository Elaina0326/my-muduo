// test_threadpool.cc —— ThreadPool 单元测试
//
// 重点验证两件事：
//   ① 提交的任务一个不丢
//   ② stop() 是"优雅停止"—— 先把队列里的任务做完，再退出线程

#include "muduo/base/Atomic.h"
#include "muduo/base/CountDownLatch.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Mutex.h"
#include "muduo/base/ThreadPool.h"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


// ---------------------------------------------------------------------------
// 基本：任务全部执行
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, RunsAllSubmittedTasks) {
    muduo::ThreadPool pool("Basic");
    pool.start(4);

    muduo::AtomicInt32 count;
    for (int i = 0; i < 1000; ++i) {
        pool.run([&count] { count.increment(); });
    }
    pool.stop();

    EXPECT_EQ(1000, count.get());
}


TEST(ThreadPoolTest, RunsTaskExactlyOnce) {
    muduo::ThreadPool pool("Once");
    pool.start(3);

    muduo::AtomicInt32 count;
    for (int i = 0; i < 500; ++i) {
        pool.run([&count] { count.increment(); });
    }
    pool.stop();

    EXPECT_EQ(500, count.get());     // 不是 499 也不是 501
}


// ---------------------------------------------------------------------------
// 确实用了多个线程
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, UsesConfiguredThreadCount) {
    constexpr int kThreads = 4;

    muduo::ThreadPool pool("Multi");
    pool.start(kThreads);

    std::set<pid_t> tids;
    muduo::MutexLock mutex;                  // set 不是线程安全的
    muduo::CountDownLatch done(200);

    for (int i = 0; i < 200; ++i) {
        pool.run([&] {
            {
                muduo::MutexLockGuard lock(mutex);
                tids.insert(muduo::CurrentThread::tid());
            }
            done.countDown();
        });
    }
    done.wait();
    pool.stop();

    EXPECT_EQ(static_cast<size_t>(kThreads), tids.size());   // 正好用了 4 个线程
    EXPECT_EQ(0u, tids.count(muduo::CurrentThread::tid()));  // 且都不是主线程
}


// ---------------------------------------------------------------------------
// ★ stop() 的优雅停止
// ---------------------------------------------------------------------------

// 关键测试：慢任务制造大量积压，然后立刻 stop()。
// 队列里剩余的任务必须【全部做完】，不能被丢掉。
TEST(ThreadPoolTest, StopDrainsPendingTasks) {
    muduo::ThreadPool pool("Drain");
    pool.start(2);                          // 只有 2 个 worker，制造积压

    muduo::AtomicInt32 count;
    for (int i = 0; i < 300; ++i) {
        pool.run([&count] {
            muduo::CurrentThread::sleepUsec(200);   // 每个任务 0.2ms
            count.increment();
        });
    }

    pool.stop();                            // 立刻停 —— 队列里还有一大堆
    EXPECT_EQ(300, count.get());            // 一个都不能丢
}


TEST(ThreadPoolTest, SubmitAfterStopIsDropped) {
    muduo::ThreadPool pool("AfterStop");
    pool.start(2);

    muduo::AtomicInt32 count;
    pool.run([&count] { count.increment(); });
    pool.stop();
    EXPECT_EQ(1, count.get());

    pool.run([&count] { count.increment(); });   // 停止后再提交
    muduo::CurrentThread::sleepUsec(50000);
    EXPECT_EQ(1, count.get());                   // 不会被执行的
}


// ⚠️ stop() 的契约是「只调一次」—— 和上游 muduo 一致。
//
//   原因：stop() 里会对 threads_ 逐个 join()，而 Thread::join() 里
//   有 assert(!joined_)。第二次 stop() 会对着已 join 过的线程再 join 一次
//   → 断言失败 abort。
//
//   为什么这样设计还行得通：正常用法只有两种 ——
//     ① 显式 stop() 一次
//     ② 让析构函数代劳（~ThreadPool 里有 if (running_) 守卫）
//   两者都不会重复调用。调用方负责不要 stop() 两次。
//
//   下面这条验证的是【真正有保证】的性质：
//   显式 stop() 之后析构，不会再 stop 一次。
TEST(ThreadPoolTest, ExplicitStopThenDestructIsSafe) {
    muduo::AtomicInt32 count;
    {
        muduo::ThreadPool pool("Dtor");
        pool.start(2);
        pool.run([&count] { count.increment(); });
        pool.stop();               // 显式停一次
    }                              // ← 析构：绝不能在这里再 join 一次
    EXPECT_EQ(1, count.get());
}


// 相反的方向：从来没 start() 过就析构，也不该出问题
TEST(ThreadPoolTest, DestructWithoutStartIsSafe) {
    muduo::ThreadPool pool("NeverStarted");
    SUCCEED();                     // running_ 是 false，析构不会碰 threads_
}


// ---------------------------------------------------------------------------
// 退化模式：线程数为 0
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, ZeroThreadsRunsInline) {
    muduo::ThreadPool pool("Inline");
    pool.start(0);

    muduo::AtomicInt32 count;
    pool.run([&count] { count.increment(); });

    EXPECT_EQ(1, count.get());      // 同步执行 —— run() 返回时已经跑完
    pool.stop();
}


TEST(ThreadPoolTest, RunBeforeStartRunsInline) {
    muduo::ThreadPool pool("NoStart");
    // 没有调用 start()，threads_ 为空 → 退化为同步执行

    muduo::AtomicInt32 count;
    pool.run([&count] { count.increment(); });
    EXPECT_EQ(1, count.get());
}


// ---------------------------------------------------------------------------
// 线程初始化回调
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, ThreadInitCallbackRunsOncePerThread) {
    constexpr int kThreads = 3;

    muduo::ThreadPool pool("Init");
    muduo::AtomicInt32 inited;
    muduo::CountDownLatch latch(kThreads);

    pool.setThreadInitCallback([&] {
        inited.increment();
        latch.countDown();
    });
    pool.start(kThreads);

    // ★ start() 只保证 tid 就绪，不保证工作线程函数已执行 —— 必须显式等
    latch.wait();
    EXPECT_EQ(kThreads, inited.get());

    pool.stop();
}


// ---------------------------------------------------------------------------
// queueSize()
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, QueueSizeIsObservable) {
    muduo::ThreadPool pool("Size");

    muduo::CountDownLatch release(1);
    muduo::CountDownLatch started(1);

    pool.start(1);
    pool.run([&] {
        started.countDown();
        release.wait();          // 占住唯一的 worker
    });
    started.wait();

    for (int i = 0; i < 10; ++i) {
        pool.run([] {});         // 都堆在队列里
    }
    EXPECT_EQ(10u, pool.queueSize());

    release.countDown();
    pool.stop();
    EXPECT_EQ(0u, pool.queueSize());     // 停止后队列已排空
}


// ---------------------------------------------------------------------------
// ★ 有界队列：队列满时 run() 必须阻塞生产者
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, MaxQueueSizeBlocksProducer) {
    muduo::ThreadPool pool("Bounded");
    pool.setMaxQueueSize(2);
    pool.start(1);                        // 只有一个 worker

    muduo::CountDownLatch firstStarted(1);
    muduo::CountDownLatch releaseFirst(1);

    // 任务 1：占住唯一的 worker，让后面的任务只能堆在队列里
    pool.run([&] {
        firstStarted.countDown();
        releaseFirst.wait();
    });
    firstStarted.wait();

    // 此时 worker 忙、队列为空。再提交 2 个任务把队列填满。
    muduo::AtomicInt32 done;
    pool.run([&done] { done.increment(); });   // 队列 [1]
    pool.run([&done] { done.increment(); });   // 队列 [1, 2]

    // 第 3 次提交时队列已满（2/2），run() 应该阻塞在 notFull_.wait()
    muduo::AtomicInt32 producerStarted;
    muduo::AtomicInt32 thirdReturned;
    std::thread producer([&] {
        producerStarted.set(1);
        pool.run([&done] { done.increment(); });
        thirdReturned.set(1);                  // run() 返回后才置位
    });

    // 等生产者确实进入 run()，再给它足够时间证明自己没返回
    while (producerStarted.get() == 0) {
        muduo::CurrentThread::sleepUsec(1000);
    }
    muduo::CurrentThread::sleepUsec(200 * 1000);   // 200ms

    EXPECT_EQ(0, thirdReturned.get())
        << "队列已满时 run() 不应该立即返回";

    // 放行 worker：它取走一个任务后会 notify notFull_，生产者随之解除阻塞
    releaseFirst.countDown();
    producer.join();

    EXPECT_EQ(1, thirdReturned.get()) << "队列腾出位置后 run() 应该返回";

    pool.stop();
    EXPECT_EQ(3, done.get());   // 3 个排队任务都要执行
}


// ---------------------------------------------------------------------------
// ★ 任务抛异常：只记录，不能 terminate 进程，也不能减少 worker
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, TaskThrowingDoesNotKillProcess) {
    muduo::ThreadPool pool("Throw");
    pool.start(1);

    // 第 1 个任务抛异常，runInThread 里会被 catch 并记录
    pool.run([] { throw std::runtime_error("boom"); });

    // 第 2 个任务必须仍然被执行 —— 如果异常逃出线程入口，
    // 进程会直接 std::terminate，这个用例根本走不到断言。
    muduo::AtomicInt32 ok;
    pool.run([&ok] { ok.increment(); });

    // 有界等待（1 秒），避免实现退化成"worker 静默退出"时测试无限挂起
    for (int i = 0; i < 100 && ok.get() == 0; ++i) {
        muduo::CurrentThread::sleepUsec(10 * 1000);
    }

    EXPECT_EQ(1, ok.get()) << "抛异常之后 worker 应该继续处理后续任务";

    pool.stop();
}


// ---------------------------------------------------------------------------
// 压力：多轮提交
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, ManySmallBatches) {
    muduo::ThreadPool pool("Stress");
    pool.start(4);

    muduo::AtomicInt32 count;
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 100; ++i) {
            pool.run([&count] { count.increment(); });
        }
    }
    pool.stop();

    EXPECT_EQ(20 * 100, count.get());
}
