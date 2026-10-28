// test_eventloopthread.cc —— Day 18：EventLoopThread / EventLoopThreadPool
//
// 覆盖：
//   · startLoop() 返回的指针有效，且 loop 在独立线程里跑
//   · ThreadInitCallback 在 loop 线程里被调用
//   · 线程池的数量、轮询、零线程退化

#include <gtest/gtest.h>

#include <future>
#include <thread>

#include "muduo/net/EventLoop.h"
#include "muduo/net/EventLoopThread.h"
#include "muduo/net/EventLoopThreadPool.h"

using namespace muduo::net;

// ---------------------------------------------------------------------------
// 辅助：把一段代码投递到 base loop 线程里执行，并等它跑完
//
// EventLoopThreadPool::start() / getNextLoop() 都要求
// baseLoop_->assertInLoopThread()，所以测试主体不能留在 main 线程里跑。
// ---------------------------------------------------------------------------
namespace {

template <typename Fn>
void runInLoopAndWait(EventLoop* loop, Fn fn) {
    std::promise<void> done;
    auto future = done.get_future();
    loop->runInLoop([&fn, &done] {
        fn();
        done.set_value();
    });
    future.wait();
}

}  // namespace

// ---------------------------------------------------------------------------
// EventLoopThread
// ---------------------------------------------------------------------------

TEST(EventLoopThreadTest, StartLoopReturnsValidPointer) {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();

    // ★ 核心断言：不是 nullptr，且能在里面执行任务
    ASSERT_NE(loop, nullptr);

    std::promise<bool> inLoopThread;
    auto future = inLoopThread.get_future();
    loop->queueInLoop(
        [loop, &inLoopThread] { inLoopThread.set_value(loop->isInLoopThread()); });

    EXPECT_TRUE(future.get());
}

TEST(EventLoopThreadTest, LoopRunsInSeparateThread) {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    ASSERT_NE(loop, nullptr);

    std::promise<std::thread::id> loopThreadId;
    auto future = loopThreadId.get_future();
    loop->queueInLoop(
        [&loopThreadId] { loopThreadId.set_value(std::this_thread::get_id()); });

    EXPECT_NE(future.get(), std::this_thread::get_id());
}

TEST(EventLoopThreadTest, InitCallbackRuns) {
    std::promise<bool> called;
    auto future = called.get_future();

    EventLoopThread t([&called](EventLoop* loop) {
        // 回调里拿到的指针必须有效，而且此刻已经身处 loop 线程
        called.set_value(loop != nullptr && loop->isInLoopThread());
    });

    EventLoop* loop = t.startLoop();
    ASSERT_NE(loop, nullptr);
    EXPECT_TRUE(future.get());
}

// ---------------------------------------------------------------------------
// EventLoopThreadPool
//
// ★ base loop 也交给 EventLoopThread —— EventLoop 要求
//   「创建它的线程 == 运行它的线程」，不能在 main 里 new 了再丢给别的线程跑。
// ---------------------------------------------------------------------------

TEST(EventLoopThreadPoolTest, CreatesNLoops) {
    EventLoopThread baseThread;
    EventLoop* base = baseThread.startLoop();
    ASSERT_NE(base, nullptr);

    EventLoopThreadPool pool(base, "pool");
    pool.setThreadNum(3);

    size_t count = 0;
    runInLoopAndWait(base, [&] {
        pool.start();
        count = pool.getAllLoops().size();
    });

    EXPECT_EQ(count, 3u);
}

TEST(EventLoopThreadPoolTest, RoundRobin) {
    EventLoopThread baseThread;
    EventLoop* base = baseThread.startLoop();
    ASSERT_NE(base, nullptr);

    EventLoopThreadPool pool(base, "pool");
    pool.setThreadNum(3);

    bool wrapped = false;
    runInLoopAndWait(base, [&] {
        pool.start();
        EventLoop* first = pool.getNextLoop();
        EventLoop* last = first;
        // 总共取 N+1 次，最后一次应回到第一个
        for (int i = 0; i < 3; ++i) {
            last = pool.getNextLoop();
        }
        wrapped = (last == first);
    });

    EXPECT_TRUE(wrapped);
}

TEST(EventLoopThreadPoolTest, ZeroThreadsFallsBackToBaseLoop) {
    EventLoopThread baseThread;
    EventLoop* base = baseThread.startLoop();
    ASSERT_NE(base, nullptr);

    EventLoopThreadPool pool(base, "pool");
    pool.setThreadNum(0);

    bool same = false;
    runInLoopAndWait(base, [&] {
        pool.start();
        same = (pool.getNextLoop() == base);
    });

    EXPECT_TRUE(same);
}
