#pragma once

#include<set>
#include<utility>
#include<vector>

#include "Channel.h"
#include "Timer.h"
#include "TimerId.h"
#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class EventLoop;

        class TimerQueue:noncopyable{
            public:
                explicit TimerQueue(EventLoop *loop);
                ~TimerQueue();

                TimerId addTimer(TimerCallback cb, Timestamp when, double interval);

                void cancel(TimerId timetid);

            private:
                using Entry = std::pair<Timestamp, Timer *>;
                using TimerList = std::set<Entry>;

                using ActiveTimer = std::pair<Timer *, int64_t>;
                using ActiveTimerSet = std::set<ActiveTimer>;

                void addTimerInLoop(Timer *timer);
                void cancelInLoop(TimerId timerId);

                void handleRead();

                std::vector<Entry> getExpired(Timestamp now);
                void reset(const std::vector<Entry> &expired, Timestamp now);

                bool insert(Timer *timer);

                EventLoop *loop_;
                const int timerfd_;
                Channel timerfdChannel_;
                TimerList timers_;

                ActiveTimerSet activeTimers_;

                bool callingExpiredTimers_ = false;
                ActiveTimerSet cancelingTimers_;
        };
        }
}