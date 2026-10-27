#pragma once
#include<pthread.h>

#include<cerrno>
#include<cstdint>
#include<ctime>

#include"Mutex.h"
#include"noncopyable.h"

namespace muduo{
    class Condition:noncopyable{
        public:
        explicit Condition(MutexLock&mutex):mutex_(mutex){
            pthread_cond_init(&pcond_, nullptr);
        }
        ~Condition(){
            pthread_cond_destroy(&pcond_);
        }

        void wait()
        {
            assert(mutex_.isLockByThisThread()); 
            MutexLock::UnassignGuard ug(mutex_);
            pthread_cond_wait(&pcond_, mutex_.getPthreadMutex());
        }
        void notify(){
            pthread_cond_signal(&pcond_);
        }

        void notifyAll(){
            pthread_cond_broadcast(&pcond_);
        }

        bool waitForSeconds(double seconds){
            struct timespec abstime;
            clock_gettime(CLOCK_REALTIME, &abstime);

            const int64_t kNanoSecondsPerSecond = 1000000000;
            int64_t nanoseconds = static_cast<int64_t>(seconds * kNanoSecondsPerSecond);

            abstime.tv_sec += static_cast<time_t>((abstime.tv_nsec + nanoseconds) / kNanoSecondsPerSecond);

            abstime.tv_nsec = static_cast<long>((abstime.tv_nsec + nanoseconds) % kNanoSecondsPerSecond);
            MutexLock::UnassignGuard ug(mutex_);
            return ETIMEDOUT == pthread_cond_timedwait(&pcond_, mutex_.getPthreadMutex(), &abstime);
        }

        private:
            MutexLock &mutex_;
            pthread_cond_t pcond_;
    };
}