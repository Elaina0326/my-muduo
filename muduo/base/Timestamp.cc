#include "Timestamp.h"

#include <sys/time.h>

#include <cstdio>

namespace muduo{
    std::string Timestamp::toString()const{
        char buf[32] = {0};
        int64_t seconds = microSecondsSinceEpoch_ / kMicroSecond_Per_Second;
        int64_t microseconds = microSecondsSinceEpoch_ % kMicroSecond_Per_Second;
        std::snprintf(buf, sizeof(buf), "%lld.%06lld",
                      static_cast<long long>(seconds),
                      static_cast<long long>(microseconds));
        return buf;
    }
    std::string Timestamp::toFormattedString(bool showMicroSeconds) const
    {
        time_t seconds = static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecond_Per_Second);
        struct tm tm_time{};
        if (::gmtime_r(&seconds, &tm_time) == nullptr)
        {
            return "invalid(" + toString() + ")";
        }
        char buf[64] = {0};

        if(showMicroSeconds){
            int microseconds = static_cast<int>(microSecondsSinceEpoch_ % kMicroSecond_Per_Second);
            std::snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d.%06d", tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, microseconds);
        }else{

            std::snprintf(buf, sizeof(buf), "%4d%02d%02d %02d:%02d:%02d", tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
        }
        return buf;
    }
    Timestamp Timestamp::now(){
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        int64_t seconds = tv.tv_sec;
        return Timestamp(seconds * kMicroSecond_Per_Second + tv.tv_usec);
    }
}