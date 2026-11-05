#pragma once

#include <cstdint>
#include <functional>

namespace muduo
{
    namespace net
    {
        class Timer;
        using TimerCallback = std::function<void()>;

        class TimerId
        {
        public:
            TimerId() : timer_(nullptr), sequence_(0) {}
            TimerId(Timer *timer, int64_t seq) : timer_(timer), sequence_(seq) {}
            friend class TimerQueue;

        private:
            Timer *timer_;
            int64_t sequence_;
        };
    }
}