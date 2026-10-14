#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "TimerId.h"
#include "muduo/base/Atomic.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Mutex.h"
#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo
{
    class Timestamp;
    namespace net
    {
        class Channel;
        class Poller;
        class TimerQueue;

        class EventLoop : noncopyable
        {
        public:
            using Functor = std::function<void()>;

            EventLoop();
            ~EventLoop();

            void loop();
            void quit();

            Timestamp pollReturnTime() const
            {
                return pollReturnTime_;
            }

            void runInLoop(Functor cb);

            void queueInLoop(Functor cb);

            void runInLoopSync(Functor cb);

            size_t queueSize() const;

            TimerId runAt(Timestamp time, TimerCallback cb);

            TimerId runAfter(double delay, TimerCallback cb);

            TimerId runEvery(double interval, TimerCallback cb);

            void cancel(TimerId timerId);

            void updateChannel(Channel *channel);
            void removeChannel(Channel *channel);
            bool hasChannel(Channel *channel);

            void assertInLoopThread()
            {
                if (!isInLoopThread())
                {
                    abortNotInLoopThread();
                }
            }

            bool isInLoopThread() const
            {
                return threadId_ == CurrentThread::tid();
            }

            bool callingPendingFunctors() const
            {
                return callingPendingFunctors_;
            }

            void wakeup();

            void ownObject(const std::shared_ptr<void> &obj)
            {
                ownedObjects_.push_back(obj);
            }

            static EventLoop *getEventLoopOfCurrentThread();

        private:
            void abortNotInLoopThread();
            void handleRead();

            void doPendingFunctors();

            using ChannelList = std::vector<Channel *>;
            std::vector<std::shared_ptr<void>> ownedObjects_;
            bool looping_ = false;

            AtomicInt32 quit_{0};
            bool eventHandling_ = false;
            bool callingPendingFunctors_ = false;

            const pid_t threadId_;
            Timestamp pollReturnTime_;

            std::unique_ptr<Poller> poller_;
            std::unique_ptr<TimerQueue> timerQueue_;

            int wakeupFd_;
            std::unique_ptr<Channel> wakeupChannel_;

            ChannelList activeChannels_;

            mutable MutexLock mutex_;
            std::vector<Functor> pendingFunctors_;
        };
    }
    using net::EventLoop;
}