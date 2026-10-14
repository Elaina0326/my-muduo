#pragma once

#include<cstdint>

#include "TimerId.h"
#include "muduo/base/Atomic.h"
#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class Timer :noncopyable{
            public:
            Timer(TimerCallback cb,Timestamp when,double interval):callback_(std::move(cb)),expiration_(when),interval_(interval),repeat_(interval_>0.0),sequence_(s_numCreated_.incrementAndGet()) {}
            void run()const{
                if(callback_){
                    callback_();
                }
            }
            Timestamp expiration()const{
                return expiration_;
            }
            bool repeat()const{
                return repeat_;
            }
            int64_t sequence()const{
                return sequence_;
            }
            void restart(Timestamp now){
                if(repeat_){
                    expiration_ = addTime(now, interval_);
                }else{
                    expiration_ = Timestamp::invalid();
                }
            }

            static int64_t numCreated(){
                return s_numCreated_.get();
            }

            private:
                const TimerCallback callback_;
                Timestamp expiration_;
                const double interval_;
                const bool repeat_;
                const int64_t sequence_;

                static AtomicInt64 s_numCreated_;
        };
    }
}