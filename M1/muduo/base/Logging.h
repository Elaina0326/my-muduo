#pragma once

#include<cerrno>
#include<sstream>
#include<string>

#include"CurrentThread.h"
#include"Timestamp.h"
#include"noncopyable.h"

namespace muduo{
    enum LogLevel{TRACE,DEBUG,INFO,WARN,ERROR,FATAL,NUM_LOG_LEVELS
    };

    class LogStream:noncopyable{
    public:
        template <typename T>
        LogStream& operator<<(const T&v){
            oss_ << v;
            return *this;
        }
        std::string str() const { return oss_.str(); }
        private:
            std::ostringstream oss_;
    };

    class Logger:noncopyable{
        public:
            Logger(const char *file, int line, LogLevel level);

            Logger(const char *file, int line, LogLevel level, int savedError);
            ~Logger();

            LogStream&stream(){
                return stream_;
            }
            static LogLevel logLevel();

            static void setLogLevel(LogLevel level);

            using OutputFunc = void (*)(const char *msg, int len);

            // ⚠️ 必须在打第一条日志【之前】调用（进程启动阶段）。
            //
            //    本实现【没有】给 g_output 加锁 —— 这是有意的取舍，不是疏漏：
            //      · 它是"启动期配置"，不是运行期可以随时切换的旋钮；
            //      · 启动阶段是单线程的，不需要同步；
            //      · 日志是热点路径，每打一条都过一遍锁，开销不划算。
            //
            //    如果确实需要在运行期切换日志出口，必须自己额外同步 ——
            //    否则会与正在打日志的线程构成数据竞争（函数指针非原子，
            //    理论上可能跳到非法地址）。
            //
            //    （上游 muduo 在这里同样没有加锁；本项目的 g_logLevel 用
            //      AtomicInt32，只是为了让 setLogLevel 更宽松一点。
            //      见 docs/postmortem.md 里关于"把隐含假设写成契约"的讨论。）
            static void setOutput(OutputFunc func);

        private:
            LogStream stream_;
            const char *file_;
            int line_;
            LogLevel level_;
            int savedError_ = 0;
    };

    
}


#define LOG_TRACE \
    for (bool _muduo_log_once =(muduo::Logger::logLevel() <= muduo::TRACE);\
     _muduo_log_once;_muduo_log_once=false )\
     muduo::Logger(__FILE__,__LINE__,muduo::TRACE).stream()
#define LOG_DEBUG                                                            \
    for (bool _muduo_log_once = (muduo::Logger::logLevel() <= muduo::DEBUG); \
         _muduo_log_once; _muduo_log_once = false)                           \
    muduo::Logger(__FILE__, __LINE__, muduo::DEBUG).stream()

#define LOG_INFO                                                           \
    for (bool _muduo_log_once = (muduo::Logger::logLevel() <= muduo::INFO); \
         _muduo_log_once; _muduo_log_once = false)                           \
    muduo::Logger(__FILE__, __LINE__, muduo::INFO).stream()

#define LOG_WARN muduo::Logger(__FILE__, __LINE__, muduo::WARN).stream()

#define LOG_ERROR muduo::Logger(__FILE__, __LINE__, muduo::ERROR).stream()

#define LOG_FATAL muduo::Logger(__FILE__, __LINE__, muduo::FATAL).stream()

#define LOG_SYSERR \
    muduo::Logger(__FILE__, __LINE__, muduo::ERROR,errno).stream()

    