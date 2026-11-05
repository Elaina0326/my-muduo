#include "CurrentThread.h"

#include<sys/syscall.h>
#include<time.h>

#include<cstdio>

namespace muduo{
    namespace CurrentThread{
         __thread int t_cachedTid=0;
         __thread char t_tidString[32];
         __thread int t_tidStringLength=6;
         __thread const char *t_threadName="unknown";

         static pid_t gettid(){
             return static_cast<pid_t>(::syscall(SYS_gettid));
         }

         void cacheTid(){
            if(t_cachedTid==0){
                t_cachedTid = gettid();
                t_tidStringLength = std::snprintf(t_tidString, sizeof(t_tidString), "%5d ", t_cachedTid);
            }
         }
         bool isMainThread(){
             return tid() == ::getpid();
         }
         
         void sleepUsec(int64_t usec){
             struct timespec ts = {0, 0};
             ts.tv_nsec = static_cast<long>(usec % 1000000 * 1000);
             ts.tv_sec = static_cast <time_t>(usec / 1000000);
             ::nanosleep(&ts, nullptr);
         }
    }
}