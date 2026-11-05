#include "EventLoopThreadPool.h"

#include <cassert>
#include<cstdio>

#include "EventLoop.h"
#include "EventLoopThread.h"

namespace muduo{
    namespace net{
        EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop,
                                                 std::string nameArg)
            : baseLoop_(baseLoop), name_(std::move(nameArg)) {}
        EventLoopThreadPool::~EventLoopThreadPool() = default;
        void EventLoopThreadPool::start(const ThreadInitCallback &cb){
            assert(!started_);
            baseLoop_->assertInLoopThread();
            started_ = true;
            for (int i = 0; i < numThreads_;++i){
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s%d", name_.c_str(), i);
                EventLoopThread *t = new EventLoopThread(cb, buf);
                threads_.push_back(std::unique_ptr<EventLoopThread>(t));
                loops_.push_back(t->startLoop());
            }
            if(numThreads_==0&&cb){
                cb(baseLoop_);
            }
        }
        EventLoop *EventLoopThreadPool::getNextLoop()
        {
            baseLoop_->assertInLoopThread();
            assert(started_);

            EventLoop *loop = baseLoop_;

            if (!loops_.empty())
            {
                loop = loops_[static_cast<size_t>(next_)];
                ++next_;
                if (static_cast<size_t>(next_) >= loops_.size())
                {
                    next_ = 0;
                }
            }
            return loop;
        }
        EventLoop *EventLoopThreadPool::getLoopForHash(size_t hashCode)
        {
            baseLoop_->assertInLoopThread();
            EventLoop *loop = baseLoop_;

            if (!loops_.empty())
            {
                loop = loops_[hashCode % loops_.size()];
            }
            return loop;
        }
        std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
        {
            baseLoop_->assertInLoopThread();
            assert(started_);

            if (loops_.empty())
            {
                return std::vector<EventLoop *>(1, baseLoop_);
            }
            return loops_;
        }
    }
}