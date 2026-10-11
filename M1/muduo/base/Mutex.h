#pragma once

#include<pthread.h>
#include<cassert>

#include"CurrentThread.h"
#include"noncopyable.h"

namespace muduo{
    class MutexLock :noncopyable{
        public:
        MutexLock(){
            pthread_mutex_init(&mutex_, nullptr);
        }
        ~MutexLock(){
            assert(holder_ == 0);
            pthread_mutex_destroy(&mutex_);
        }

        void lock(){
            pthread_mutex_lock(&mutex_);
            assignHolder();
        }

        void unlock(){
            unassignHolder();
            pthread_mutex_unlock(&mutex_);
        }

        bool isLockByThisThread()const{
            return holder_ == CurrentThread::tid();
        }

        pthread_mutex_t* getPthreadMutex(){
            return &mutex_;
        }

    private:
        friend class Condition;

        void unassignHolder()
        {
            holder_ = 0;
        }
            void assignHolder(){
                holder_ = CurrentThread::tid();
            }

            pthread_mutex_t mutex_;
            pid_t holder_ = 0;
    };

    class MutexLockGuard:noncopyable{
        public:
            explicit MutexLockGuard(MutexLock &mutex):mutex_(mutex){
                mutex_.lock();
            }
            ~MutexLockGuard(){
                mutex_.unlock();
            }

        private:
            MutexLock &mutex_;
    };
}

#define MutexLockGuard(x) \
    static_assert(false, "missing mutex guard variable name")
