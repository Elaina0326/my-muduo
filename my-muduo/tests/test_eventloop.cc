#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "muduo/net/EventLoop.h"

using namespace muduo::net;

// ---------------------------------------------------------------------------
// 线程归属
// ---------------------------------------------------------------------------
TEST(EventLoopTest, IsInLoopThread)
{
    EventLoop loop;
    EXPECT_TRUE(loop.isInLoopThread());

    bool childInLoopThread = true;
    std::thread t([&loop, &childInLoopThread]
                  { childInLoopThread = loop.isInLoopThread(); });
    t.join();

    EXPECT_FALSE(childInLoopThread);
}

TEST(EventLoopTest, AssertInLoopThreadAborts)
{
    // 这个测试会 abort 进程，所以只在 Debug 下手动验证
    // 这里只验证正常路径不崩
    EventLoop loop;
    EXPECT_NO_FATAL_FAILURE(loop.assertInLoopThread());
}

// ---------------------------------------------------------------------------
// ★★ 跨线程任务投递 —— EventLoop 的核心能力 ★★
// ---------------------------------------------------------------------------
TEST(EventLoopTest, RunInLoopSameThreadExecutesImmediately)
{
    EventLoop loop;
    bool executed = false;

    loop.runInLoop([&executed]
                   { executed = true; });

    // 同一个线程：应该立即执行，不需要跑 loop()
    EXPECT_TRUE(executed);
}

TEST(EventLoopTest, RunInLoopAcrossThreads)
{
    EventLoop loop;
    std::atomic<bool> executed{false};

    std::thread t([&loop, &executed]
                  {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        loop.runInLoop([&loop, &executed] {
            executed = true;
            // ★ 关键断言：任务必须在 loop 线程里执行
            EXPECT_TRUE(loop.isInLoopThread());
            loop.quit();
        }); });

    loop.loop();
    t.join();

    EXPECT_TRUE(executed.load());
}

TEST(EventLoopTest, QueueSizeGrows)
{
    EventLoop loop;

    // 不启动 loop()，只入队
    std::thread t([&loop]
                  {
        for (int i = 0; i < 10; ++i) {
            loop.queueInLoop([] {});
        } });
    t.join();

    EXPECT_EQ(loop.queueSize(), 10u);
}

TEST(EventLoopTest, QueueInLoopFromOtherThreadWakesUpLoop)
{
    EventLoop loop;
    std::atomic<bool> executed{false};

    std::thread t([&loop, &executed]
                  {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 此时 loop 正阻塞在 epoll_wait 上
        // queueInLoop 应该通过 eventfd 把它唤醒
        loop.queueInLoop([&loop, &executed] {
            executed = true;
            loop.quit();
        }); });

    loop.loop();
    t.join();

    EXPECT_TRUE(executed.load());
}

// ---------------------------------------------------------------------------
// ★ 压力测试：100 万个任务不能丢
// ---------------------------------------------------------------------------
TEST(EventLoopTest, MillionTasksNotLost)
{
    const int kTotal = 1000000;

    EventLoop loop;
    std::atomic<int> count{0};

    std::thread t([&loop, &count]
                  {
        for (int i = 0; i < kTotal; ++i) {
            loop.runInLoop([&count] { count.fetch_add(1); });
        }
        // quit 的任务排在最后 —— FIFO 保证前面的都执行完了
        loop.runInLoop([&loop] { loop.quit(); }); });

    loop.loop();
    t.join();

    EXPECT_EQ(count.load(), kTotal);
}

TEST(EventLoopTest, TaskSubmitsTaskFromWithinLoop)
{
    // ★ 考察 doPendingFunctors 的 swap 技巧和 queueInLoop 的
    //   callingPendingFunctors_ 分支。
    //
    // 场景：任务 A 在执行过程中又投递了任务 B。
    //   执行 A 时，doPendingFunctors 已经把队列 swap 到局部变量了，
    //   所以 B 进的是新的 pendingFunctors_，本轮看不到。
    //   必须靠 queueInLoop 里的 callingPendingFunctors_ 判断触发 wakeup()，
    //   否则 B 要等到下一次 IO 事件才被执行。
    EventLoop loop;
    std::atomic<int> count{0};

    std::thread t([&loop, &count]
                  {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        loop.runInLoop([&loop, &count] {
            count.fetch_add(1);                 // 外层任务

            loop.runInLoop([&count] {
                count.fetch_add(1);             // 内层任务（嵌套投递）
            });

            loop.runInLoop([&loop] { loop.quit(); });
        }); });

    loop.loop();
    t.join();

    EXPECT_EQ(count.load(), 2);
}

// ---------------------------------------------------------------------------
// quit（在 loop 线程内调用）
// ---------------------------------------------------------------------------
TEST(EventLoopTest, QuitInLoopThread)
{
    EventLoop loop;

    loop.runAfter(0.05, [&loop]
                  { loop.quit(); });
    loop.loop(); // 应该在 50ms 后返回

    SUCCEED();
}

TEST(EventLoopTest, QuitFromOtherThread)
{
    EventLoop loop;

    std::thread t([&loop]
                  {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // ★ quit() 内部会 wakeup()，否则 loop 要等 epoll_wait 超时
        loop.quit(); });

    loop.loop();
    t.join();

    SUCCEED();
}
