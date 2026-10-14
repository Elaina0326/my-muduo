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
        class Eventloop;

        class TimerQueue:noncopyable{
            public:
            private:
                using Entry = std::pair<Timestamp, Timer *>;
                using TimerList = std::set<Entry>;

                using ActiveTimer = std::pair<Timer *, int64_t>;
                using ActiveTimerSet = std::set<ActiveTimer>;

                void addTimerInLoop(Timer *timer);
                void cancelInLoop(TimerId timerId);

                void handleRead();

                std::vector<Entry> getExpired(Timestamp now);
        };
    }
}