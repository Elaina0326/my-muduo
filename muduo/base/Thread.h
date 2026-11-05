#pragma once

#include<pthread.h>

#include<functional>
#include<string>

#include"Atomic.h"
#include"CountDownLatch.h"
#include"noncopyable.h"

namespace muduo{
    class Thread:noncopyable{
        public:
            using ThreadFunc = std::function<void()>;
            explicit Thread(ThreadFunc func, const std::string &name = std::string());
            ~Thread();

            void start();

            int join();

            bool started()const{
                return started_;
            }

            pid_t tid()const{
                return tid_;
            }

            const std::string&name()const{
                return name_;
            }

            static int numCreated(){
                return numCreated_.get() ;
            }

        private:
            void setDefaultName();

            bool started_ = false;
            bool joined_ = false;
            pthread_t pthreadId_ = 0;
            pid_t tid_ = 0;
            ThreadFunc func_;
            std::string name_;

            CountDownLatch latch_;

            static AtomicInt32 numCreated_;
    };
}