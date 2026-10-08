#pragma once 
#include<ctime>
#include<cstdint>
#include<string>

namespace muduo{
    class Timestamp{
public:
    static const int kMicroSecond_Per_Second = 1000 * 1000;
    Timestamp():microSecondsSinceEpoch_(0){}
    explicit Timestamp(int64_t microSecondsSinceEpoch)
        :microSecondsSinceEpoch_(microSecondsSinceEpoch){}
    void swap(Timestamp& that){
        std::swap(microSecondsSinceEpoch_, that.microSecondsSinceEpoch_);
    }

    std::string toString() const;
    std::string toFormattedString(bool showMicroSeconds = true) const;

    bool valid()const{
        return microSecondsSinceEpoch_ > 0;
    }

    int64_t microSecondsSinceEpoch()const{
        return microSecondsSinceEpoch_;
    }

    time_t secondsSinceEpoch()const{
        return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecond_Per_Second);
    }

    static Timestamp now();
    
    static Timestamp invalid(){
        return Timestamp();
    }

    static Timestamp fromUnixTime(time_t t){
        return fromUnixTime(t, 0);
    }

    static Timestamp fromUnixTime(time_t t,int microseconds)
    {
        return Timestamp(static_cast<int64_t>(t) * kMicroSecond_Per_Second + microseconds);
    }

private:
    int64_t microSecondsSinceEpoch_;
};

inline bool operator<(Timestamp lhs,Timestamp rhs){
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs)
{
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

inline double timeDiff(Timestamp high, Timestamp low)
{
    int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecond_Per_Second;
}
inline Timestamp addtime(Timestamp timestamp_, double seconds)
{
    int64_t delta =  static_cast<int64_t>(seconds*Timestamp::kMicroSecond_Per_Second) ;
    return Timestamp(timestamp_.microSecondsSinceEpoch()+delta);
}
}