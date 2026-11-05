#include "Logging.h"

#include<unistd.h>

#include<cstdio>
#include<cstdlib>
#include<cstring>

#include"Atomic.h"

namespace muduo{
    namespace{
        AtomicInt32 g_logLevel(INFO);
        const char* levelName(LogLevel level){
            switch(level){
                case TRACE :
                    return "TRACE";
                case DEBUG:
                    return "DEBUG";
                case INFO:
                    return "INFO";
                case WARN:
                    return "WARN";
                case ERROR:
                    return "ERROR";
                case FATAL:
                    return "FATAL";
                default:
                    return "UNKNOW";
                }
        }

        void defaultOutput(const char*msg,int len){
            ssize_t n = ::write(STDOUT_FILENO, msg, static_cast<size_t>(len));
            (void)n;
        }
        Logger::OutputFunc g_output = defaultOutput;

        const char* strerrorTl(int err){
            static thread_local char buf[256];
            return ::strerror_r(err, buf, sizeof(buf));
        }
    }
    LogLevel Logger::logLevel(){
        return static_cast<LogLevel>(g_logLevel.get());
    }

    void Logger::setLogLevel(LogLevel level){
        g_logLevel.set(level);
    }

    void Logger::setOutput(OutputFunc func){
        g_output = func;
    }
    Logger::Logger(const char *file, int line, LogLevel level) : file_(file), line_(line), level_(level){}

    Logger::Logger(const char *file, int line, LogLevel level,int savedErrno ) : file_(file), line_(line), level_(level) ,savedError_(savedErrno) {}

    Logger::~Logger(){
        std::string msg = stream_.str();
        while(!msg.empty()&&(msg.back()=='\n'||msg.back()=='\r')){
            msg.pop_back();
        }
            const char *base = std::strrchr(file_, '/');
            const char *filename = base ? base + 1 : file_;

            std::string errSuffix;
            if(savedError_!=0){
                errSuffix = ": ";
                errSuffix += strerrorTl(savedError_);
                errSuffix += " (errno=";
                errSuffix += std::to_string(savedError_);
                errSuffix += ")";
            }
            char buf[1024];
            int n = std::snprintf(
                buf, sizeof(buf), "%s %5d %s %s:%d] %s%s\n",
                Timestamp::now().toString().c_str(), CurrentThread::tid(), levelName(level_), filename, line_, msg.c_str(), errSuffix.c_str());
            if(n>0){
                g_output(buf, n < static_cast<int>(sizeof(buf))
                                  ? n
                                  : static_cast<int>(sizeof(buf)) - 1);
            }
            if(level_==FATAL){
                fflush(stdout);
                abort();
            }
        
    }
}