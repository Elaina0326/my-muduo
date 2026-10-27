
#pragma once


#include<deque>
#include<functional>
#include<memory>
#include<string>
#include<vector>

#include "Condition.h"
#include "Mutex.h"
#include"Thread.h"
#include"noncopyable.h"

namespace muduo{
    class ThreadPool:noncopyable{
        public:
            using Task = std::function<void()>;
            explicit ThreadPool(const std::string &nameArg = std::string("ThreadPool"));

            ~ThreadPool();
        void setMaxQueueSize(size_t maxSize)
        {
            assert(threads_.empty());
            maxQueueSize_ = maxSize;
        }
            void setThreadInitCallback(const Task&cb){
                threadInitCallback_ = cb;
            }

            void start(int numThreads);

            void stop();

            const std::string &name() const { return name_; }

            size_t queueSize() const;
            void run(Task f);

        private:
            void runInThread();
            bool take(Task *task);

            mutable MutexLock mutex_;
            Condition notEmpty_;
            Condition notFull_;
            std::string name_;
            Task threadInitCallback_;
            std::vector<std::unique_ptr<muduo::Thread>> threads_;
            std::deque<Task> queue_;
            size_t maxQueueSize_ = 0;
            bool running_ = false;
    };
}
