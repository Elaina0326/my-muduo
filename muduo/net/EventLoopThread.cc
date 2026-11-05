#include "EventLoopThread.h"

#include "EventLoop.h"

namespace muduo{
    namespace net{
        EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,const std::string &name):loop_(nullptr),thread_(std::bind(&EventLoopThread::threadFunc,this),name),cond_(mutex_),callback_(cb){}
        EventLoopThread::~EventLoopThread(){
            EventLoop *loop = nullptr;
            {
                MutexLockGuard lock(mutex_);
                loop = loop_;
                if (loop != nullptr)
                {
                    loop->quit();
                }
            }
            if (loop != nullptr)
            {
                thread_.join();
            }
        }
        EventLoop* EventLoopThread::startLoop(){
            thread_.start();
            EventLoop *loop = nullptr;
            {
                MutexLockGuard lock(mutex_);
                while(loop_==nullptr)
                {
                    cond_.wait();
                }
                loop = loop_;
            }
            return loop;
        }
        void EventLoopThread::threadFunc(){
            EventLoop loop;
            if(callback_){
                callback_(&loop);
            }
            {
                MutexLockGuard lock(mutex_);
                loop_ = &loop;
                cond_.notify();
            }
            loop.loop();
            MutexLockGuard lock(mutex_);
            loop_ = nullptr;
        }
    }
}