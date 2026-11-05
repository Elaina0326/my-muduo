// test_timestamp.cc —— Timestamp 单元测试
//
// 覆盖：构造、now()、toString()、toFormattedString()、fromUnixTime()、
//       比较运算符、timeDiff()、addtime()、swap()。
//
// ⚠️ toFormattedString() 内部用的是 gmtime_r（UTC，不是本地时间），
//    所以下面的期望值都是按 UTC 算的。如果以后改成 localtime_r，这两个用例要跟着改。

#include "muduo/base/Timestamp.h"

#include <gtest/gtest.h>

#include <sys/time.h>
#include <unistd.h>

namespace
{

    // 1000000000 秒 = 2001-09-09 01:46:40 UTC（固定值，可直接对拍）
    constexpr time_t kEpoch1G = 1000000000;

} // namespace

// ===========================================================================
// 构造 与 invalid()
// ===========================================================================

TEST(TimestampTest, DefaultConstructIsInvalid)
{
    muduo::Timestamp t;
    EXPECT_EQ(0, t.microSecondsSinceEpoch());
    EXPECT_FALSE(t.valid());
    EXPECT_EQ("0.000000", t.toString());
}

TEST(TimestampTest, InvalidFactory)
{
    muduo::Timestamp t = muduo::Timestamp::invalid();
    EXPECT_FALSE(t.valid());
    EXPECT_EQ(0, t.microSecondsSinceEpoch());
}

TEST(TimestampTest, ExplicitConstructRoundTrip)
{
    muduo::Timestamp t(1234567890123456LL);
    EXPECT_TRUE(t.valid());
    EXPECT_EQ(1234567890123456LL, t.microSecondsSinceEpoch());
}

TEST(TimestampTest, SecondsSinceEpochTruncates)
{
    // 秒以下直接截断
    muduo::Timestamp t(1234567890123456LL);
    EXPECT_EQ(1234567890L, static_cast<long>(t.secondsSinceEpoch()));

    // 刚好整秒
    muduo::Timestamp whole(5000000LL); // 5.000000 秒
    EXPECT_EQ(5L, static_cast<long>(whole.secondsSinceEpoch()));
}

// ===========================================================================
// now()
// ===========================================================================

TEST(TimestampTest, NowMatchesGetTimeOfDay)
{
    // now() 用的是 gettimeofday，所以结果必须夹在前后两次调用之间
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    const int64_t before = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;

    const muduo::Timestamp now = muduo::Timestamp::now();
    const int64_t t = now.microSecondsSinceEpoch();

    ::gettimeofday(&tv, nullptr);
    const int64_t after = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;

    EXPECT_GE(t, before);
    EXPECT_LE(t, after);
}

TEST(TimestampTest, NowIsMonotonic)
{
    const muduo::Timestamp a = muduo::Timestamp::now();
    ::usleep(2000); // 睡 2 毫秒
    const muduo::Timestamp b = muduo::Timestamp::now();

    EXPECT_TRUE(a < b);
    EXPECT_GE(muduo::timeDiff(b, a), 0.001); // 至少过去 1 毫秒
}

// ===========================================================================
// 格式化
// ===========================================================================

TEST(TimestampTest, ToStringFormat)
{
    EXPECT_EQ("0.000000", muduo::Timestamp().toString());
    EXPECT_EQ("1000000000.123456",
              muduo::Timestamp::fromUnixTime(kEpoch1G, 123456).toString());
    // 微秒部分必须补零到 6 位，不能输出 ".7"
    EXPECT_EQ("1000000000.000007",
              muduo::Timestamp::fromUnixTime(kEpoch1G, 7).toString());
}

TEST(TimestampTest, ToFormattedStringUtc)
{
    const muduo::Timestamp t = muduo::Timestamp::fromUnixTime(kEpoch1G, 123456);

    EXPECT_EQ("20010909 01:46:40", t.toFormattedString(false));
    EXPECT_EQ("20010909 01:46:40.123456", t.toFormattedString(true));

    // 默认参数是 true
    EXPECT_EQ(t.toFormattedString(true), t.toFormattedString());
}

// ===========================================================================
// fromUnixTime()
// ===========================================================================

TEST(TimestampTest, FromUnixTime)
{
    const muduo::Timestamp t = muduo::Timestamp::fromUnixTime(1234567890, 654321);
    EXPECT_EQ(1234567890654321LL, t.microSecondsSinceEpoch());

    // 不传微秒时默认 0
    const muduo::Timestamp whole = muduo::Timestamp::fromUnixTime(1234567890);
    EXPECT_EQ(1234567890000000LL, whole.microSecondsSinceEpoch());
}

// ===========================================================================
// 比较运算符
// ===========================================================================

TEST(TimestampTest, Comparison)
{
    const muduo::Timestamp a(100);
    const muduo::Timestamp b(200);

    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_FALSE(a < a); // 严格小于

    EXPECT_TRUE(a == muduo::Timestamp(100));
    EXPECT_FALSE(a == b);
}

// ===========================================================================
// timeDiff() / addtime()
// ===========================================================================

TEST(TimestampTest, TimeDiff)
{
    const muduo::Timestamp a(1000000); // 1.0 秒
    const muduo::Timestamp b(3500000); // 3.5 秒

    EXPECT_DOUBLE_EQ(2.5, muduo::timeDiff(b, a));
    EXPECT_DOUBLE_EQ(0.0, muduo::timeDiff(a, a));
    EXPECT_DOUBLE_EQ(-2.5, muduo::timeDiff(a, b)); // 允许负数
}

TEST(TimestampTest, AddTime)
{
    const muduo::Timestamp t(1000000); // 1.0 秒

    EXPECT_EQ(2500000LL, muduo::addTime(t, 1.5).microSecondsSinceEpoch());
    EXPECT_EQ(500000LL, muduo::addTime(t, -0.5).microSecondsSinceEpoch());
    EXPECT_EQ(1000000LL, muduo::addTime(t, 0.0).microSecondsSinceEpoch());
}

// ===========================================================================
// swap()
// ===========================================================================

TEST(TimestampTest, Swap)
{
    muduo::Timestamp a(111);
    muduo::Timestamp b(222);

    a.swap(b);

    EXPECT_EQ(222LL, a.microSecondsSinceEpoch());
    EXPECT_EQ(111LL, b.microSecondsSinceEpoch());
}
