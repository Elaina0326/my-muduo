#include"Thread.h"

#include<sys/prctl.h>

#include<cstdio>
#include<cassert>
#include<utility>

#include"CurrentThread.h"

namespace muduo{
    namespace{
        struct ThreadData{
            using ThreadFunc = Thread::ThreadFunc;

            ThreadFunc func_;
            std::string name_;
            pid_t *tid_;
            CountDownLatch *latch_;

            ThreadData(ThreadFunc func, const std::string &name, pid_t *tid, CountDownLatch *latch) : func_(std::move(func)), name_(name), tid_(tid), latch_(latch){} ;

            void runInThread(){
                *tid_ = CurrentThread::tid();
                tid_ = nullptr;
                latch_->countDown();
                latch_=nullptr;

                CurrentThread::t_threadName = name_.empty() ? "muduoThread" : name_.c_str();
                ::prctl(PR_SET_NAME, CurrentThread::t_threadName);
                func_();
                CurrentThread::t_threadName = "finished";
            }
        };
        void* startThread (void* obj){
            ThreadData *data = static_cast<ThreadData *>(obj);
            data->runInThread();
            delete data;
            return nullptr;
        }
    }
    AtomicInt32 Thread::numCreated_;

    Thread::Thread(ThreadFunc func, const std::string &n) : func_(std::move(func)), name_(n), latch_(1){
        setDefaultName();
    }
    Thread::~Thread(){
        if(started_&&!joined_){
            pthread_detach(pthreadId_);
        }
    }

    void Thread::setDefaultName(){
        int num = numCreated_.incrementAndGet();
        if(name_.empty()){
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Thread%d", num);
            name_ = buf;
        }
    }
    void Thread::start(){
        assert(!started_);
        started_ = true;

        ThreadData *data = new ThreadData(func_, name_, &tid_, &latch_);
        if(pthread_create(&pthreadId_,nullptr,&startThread,data)){
            started_ = false;
            delete data;
            perror("pthread_create");
        }else{
            latch_.wait();
            assert(tid_ > 0);
        }
    }

    int Thread::join(){
        assert(started_);
        assert(!joined_);
        joined_ = true;
        return pthread_join(pthreadId_, nullptr);
    }
}