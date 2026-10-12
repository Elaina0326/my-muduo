#pragma once

#include<cassert>
#include<deque>
#include<utility>

#include "Condition.h"
#include"noncopyable.h"
#include"Mutex.h"


namespace muduo{
    template<typename T>
    class BlockingQueue: noncopyable{
        public:
            using queue_type = std::deque<T>;

            BlockingQueue():mutex_(),notEmpty(mutex_){}  ;
            void put(const T& x){
                MutexLockGuard lock(mutex_);
                queue_.push_back(x);
                notEmpty_.notify();
            }

            void put(T&&x){
                MutexLockGuard lock(mutex_);
                queue_.push_back(std::move(x));
                notEmpty_.notify();
            }

            T take(){
                MutexLockGuard lock(mutex_);
                while(queue_.empty()){
                    notEmpty_.wait();
                }
                T front(std::move(queue_.front()));
                queue_.pop_front();
                return front;
            }

            queue_type drain(){
                queue_type queue;
                {
                    MutexLockGuard lock(mutex_);
                    queue.swap(queue_);
                    assert(queue_.empty());
                }
                return queue;
            }
            size_t size()const{
                MutexLockGuard lock(mutex_);
                return queue_.size();
            }

        private:
            mutable MutexLock mutex_;
            Condition notEmpty_;
            queue_type queue_;
    };
}
