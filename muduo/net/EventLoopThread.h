#pragma once

#include<functional>
#include<string>

#include "muduo/base/Condition.h"
#include "muduo/base/Mutex.h"
#include "muduo/base/Thread.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class EventLoop;

        class EventLoopThread:noncopyable{
        public:
            using ThreadInitCallback = std::function<void(EventLoop *)>;
            explicit EventLoopThread(
                const ThreadInitCallback &cb = ThreadInitCallback(),
                const std::string &name = std::string());
            ~EventLoopThread();
            EventLoop *startLoop();

        private:
            void threadFunc();

            EventLoop *loop_ = nullptr;
            Thread thread_;
            MutexLock mutex_;
            Condition cond_;
            ThreadInitCallback callback_;
        };
    }
}