#pragma once

#include<cstdint>
#include "noncopyable.h"

namespace muduo{
template<typename T>
class AtomicIntegerT:noncopyable{
public:
    AtomicIntegerT() : value_(0) {};
    explicit AtomicIntegerT(T num):value_(num){}
    T get()const{
        return __atomic_load_n(&value_, __ATOMIC_SEQ_CST);
    }

    T getAndAdd(T x){
        return __atomic_fetch_add(&value_, x, __ATOMIC_SEQ_CST);
    }

    T addAndGet(T x){
        return getAndAdd(x) + x;
    }
    T incrementAndGet(){
        return addAndGet(1);
    }
    T decrementAndGet (){
        return addAndGet(-1);
    }
    T getAndSet(T newValue){
        return __atomic_exchange_n(&value_, newValue, __ATOMIC_SEQ_CST);
    }
    void add(T x){
        getAndAdd(x);
    }
    void increment(){
        incrementAndGet();
    }

    void set(T newValue){
        __atomic_store_n(&value_, newValue, __ATOMIC_SEQ_CST);
    }

    private:
    volatile T value_;
};
using AtomicInt32 = AtomicIntegerT<int32_t>;
using AtomicInt64 = AtomicIntegerT<int64_t>;
}