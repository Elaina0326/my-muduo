#pragma once

#include "Condition.h"
#include "Mutex.h"
#include "noncopyable.h"

namespace muduo{
    class CountDownLatch :noncopyable{
        public:
            explicit CountDownLatch(int count) : mutex_(), cond_(mutex_), count_(count){} 
            void wait(){
                MutexLockGuard lock(mutex_);
                while(count_>0){
                    cond_.wait();
                }
            }

            void countDown(){
                MutexLockGuard lock(mutex_);
                --count_;
                if(count_==0){
                    cond_.notifyAll();
                }
            }

            int getCount()const{
                MutexLockGuard lock(mutex_);
                return count_;
            }

        private:
            mutable MutexLock mutex_;
            Condition cond_;
            int count_;
    };
}