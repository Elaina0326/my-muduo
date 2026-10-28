// ============================================================================
// test_mutex.cc —— MutexLock / MutexLockGuard 单元测试
//
// ★ 这个文件是 gtest 语法的【教学样本】，注释里讲的都是语法，不只是断言本身。
//   后面几个测试文件的注释会简略一些，因为有不懂的可以回头看这里。
// ============================================================================


// ============================================================================
// 【第 1 节】包含头文件
// ============================================================================
//
// 被测头文件 + gtest 头文件，这是每个测试文件的固定开头。
//
// 引号的区别很重要：
//   "muduo/base/Mutex.h"  双引号 → 先在【当前文件所在目录】找，再找包含路径
//   <gtest/gtest.h>       尖括号 → 只在【编译器的系统包含路径】里找
//
// 习惯：自己的代码用双引号，第三方库用尖括号。
//
// ⚠️ 这里不用写 main()！gtest_main 这个库已经提供了 main，
//    它负责扫描本文件里所有的 TEST 并挨个执行。
#include "muduo/base/Mutex.h"

#include <gtest/gtest.h>

#include <thread>   // std::thread


// ============================================================================
// 【第 2 节】TEST 宏 —— 一个用例就是一个函数
// ============================================================================
//
//     TEST(套件名, 用例名) { ... }
//
//   套件名：同一模块的用例用同一个名字。习惯写法 = 被测类名 + "Test"
//   用例名：描述这一条在验什么，最好写成一句【陈述句】
//           —— 这样失败时输出 "MutexLockTest.LockedByThisThreadWhileHolding FAILED"
//              本身就在告诉你哪条性质被破坏了
//
//   两个名字的规则：
//     · 必须是合法 C++ 标识符（字母/数字/下划线，不能数字开头）
//     · 【全文件内不能重复】—— 重复会直接编译报错
//     · 跨文件重名也会在链接期或运行期出问题，尽量起得具体
//
//   函数体就是普通 C++ 代码，返回值 void，不用写 return。
// ============================================================================


// ---------------------------------------------------------------------------
// 用例 1：默认状态下没人持锁
// ---------------------------------------------------------------------------

TEST(MutexLockTest, DefaultNotLocked) {
    // 在栈上创建一个互斥锁。此时没有任何线程持有它。
    muduo::MutexLock m;

    // ★ EXPECT_FALSE(表达式)
    //     断言表达式为【假】。为真 → 这条用例失败。
    //
    //   重要的前缀规则：
    //     EXPECT_xxx  失败后【继续执行】后面的代码
    //     ASSERT_xxx  失败后【立刻 return】，后面的代码不跑了
    //
    //   EXPECT_ 是默认选择。只有当后面几行【依赖】前面成立时才用 ASSERT_：
    //       ASSERT_NE(nullptr, ptr);     // ptr 若是空的，下一行就段错误了
    //       EXPECT_EQ(10, ptr->size());
    EXPECT_FALSE(m.isLockByThisThread());
}


// ---------------------------------------------------------------------------
// 用例 2：持锁期间 isLockByThisThread() 必须为真
//
// ★ 这一条就是抓「lock() 忘了 assignHolder()」那个 bug 的。
//   之前 holder_ 恒为 0，这条会直接变红。
// ---------------------------------------------------------------------------

TEST(MutexLockTest, LockedByThisThreadWhileHolding) {
    muduo::MutexLock m;

    // ★ 这一对大括号 { } 是【作用域】。
    //   进去了才有 lock 这个变量，出了这个 } 它就析构了。
    //   析构 → 自动调用 unlock()。这就是 RAII 的价值：
    //   不用手写 unlock，也不怕中间 return 或抛异常。
    {
        muduo::MutexLockGuard lock(m);   // 构造 = 加锁
        EXPECT_TRUE(m.isLockByThisThread());
    }                                     // ← 右括号：guard 析构 = 解锁

    EXPECT_FALSE(m.isLockByThisThread());
}


// ---------------------------------------------------------------------------
// 用例 3：RAII —— 即使中途 return，锁也会被释放
//
// 这是 MutexLockGuard 存在的全部理由。
// ---------------------------------------------------------------------------

namespace {

// 一个"会提前返回"的函数。如果手写 lock/unlock，这里的 return
// 就会跳过 unlock，造成死锁。
bool doSomethingAndReturnEarly(muduo::MutexLock& m) {
    muduo::MutexLockGuard lock(m);
    // ... 中间假设有很多逻辑 ...
    return true;    // ← 直接返回，没有写 unlock。但因为 RAII，锁照样释放了。
}

}  // namespace

TEST(MutexLockGuardTest, UnlocksEvenOnEarlyReturn) {
    muduo::MutexLock m;

    EXPECT_TRUE(doSomethingAndReturnEarly(m));
    EXPECT_FALSE(m.isLockByThisThread());    // 锁已经释放了

    // 再锁一次，确认锁没有被卡住（如果上次没释放，这里会直接死锁，
    // 用例会因超时失败 —— ctest 里设了 TIMEOUT 60 秒）
    {
        muduo::MutexLockGuard lock(m);
        EXPECT_TRUE(m.isLockByThisThread());
    }
}


// ---------------------------------------------------------------------------
// 用例 4：互斥真的生效 —— 用普通变量做并发自增
//
// 这是 MutexLock 存在的唯一理由。
//
// 注意这里的 counter 是【普通的 long，不是原子类型】。
// 4 个线程各自增 10 万次，如果没有互斥保护，
// "读-改-写"三步会交错，最终结果一定小于 40 万。
// ---------------------------------------------------------------------------

TEST(MutexLockTest, MutualExclusionProtectsPlainVariable) {
    muduo::MutexLock m;
    long counter = 0;                    // ★ 故意用普通 long

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100000;

    // std::thread 数组，先默认构造，稍后再赋值
    std::thread threads[kThreads];

    for (int i = 0; i < kThreads; ++i) {
        // ★ [](){...} 是 lambda —— 一个能当函数传的匿名函数
        //   [&m, &counter] 叫【捕获列表】：
        //      [&]  按引用捕获（能改外面的变量）
        //      [=]  按值捕获（拷贝一份，改了不影响外面）
        //   这里要改 counter，所以必须按引用捕获。
        threads[i] = std::thread([&m, &counter] {
            for (int j = 0; j < kPerThread; ++j) {
                // 每次自增都持锁。guard 的作用域就是这一层 for 循环体，
                // 每轮循环结束都会析构一次并解锁。
                muduo::MutexLockGuard lock(m);
                ++counter;
            }
        });
    }

    // join = 等这个线程跑完。不 join 的话主线程可能先退出，
    // 测试还没跑完就结束了。
    for (auto& t : threads) {
        t.join();
    }

    // ★ EXPECT_EQ(期望值, 实际值)
    //     顺序不要写反 —— 它决定了失败信息怎么读：
    //          Expected equality of these values:
    //            400000                 ← 第 1 个参数（你的期望）
    //            kThreads * kPerThread
    //              Which is: 399998     ← 第 2 个参数的实际值
    //     一眼就能看出差多少。
    EXPECT_EQ(kThreads * kPerThread, counter);
}


// ============================================================================
// 【第 3 节】常用断言宏速查
// ============================================================================
//
//   相等类
//     EXPECT_EQ(a, b)         a == b
//     EXPECT_NE(a, b)         a != b
//     EXPECT_LT / LE / GT / GE   小于 / 小于等于 / 大于 / 大于等于
//
//   布尔类
//     EXPECT_TRUE(x)          x 为真
//     EXPECT_FALSE(x)         x 为假
//
//   字符串类（比较 const char* 时【必须】用这两个，不要用 EQ）
//     EXPECT_STREQ(s1, s2)    两个 C 字符串内容相同
//     EXPECT_STRNE(s1, s2)    内容不同
//
//   浮点类（浮点不能直接用 EQ 比！）
//     EXPECT_DOUBLE_EQ(a, b)      精确到 ULP 的相等
//     EXPECT_NEAR(a, b, eps)      |a - b| <= eps
//
//   异常类
//     EXPECT_THROW(语句, 异常类型)
//     EXPECT_NO_THROW(语句)
//
// 把 EXPECT_ 换成 ASSERT_ 就是"失败即返回"版本。
//
// ============================================================================
// 【第 4 节】失败信息怎么读
// ============================================================================
//
// 跑 ctest 看到失败时，直接跑二进制能看到更详细的输出：
//
//     ./build/unit_tests --gtest_filter=MutexLockTest.*
//
// 典型输出：
//
//     [ RUN      ] MutexLockTest.MutualExclusionProtectsPlainVariable
//     test_mutex.cc:118: Failure
//     Expected equality of these values:
//       400000                     ← 你写在第 1 个参数的
//       counter
//         Which is: 399998         ← 实际值
//     [  FAILED  ] ...
//
// 几个常用的命令行参数：
//     --gtest_filter=套件名.*            只跑某个套件
//     --gtest_filter=套件名.用例名        只跑一条
//     --gtest_filter=-MutexLockTest.*    排除某个套件（减号 = 排除）
//     --gtest_list_tests                列出所有用例名
//     --gtest_repeat=100                重复跑 100 次（并发测试很有用！）
//     --gtest_shuffle                   打乱顺序跑
//
// ★ --gtest_repeat=100 特别适合并发测试：
//   数据竞争是概率性的，跑一次通过不代表没问题。
// ============================================================================
