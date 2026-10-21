#pragma once

#include <string>

#include "Callbacks.h"
#include "InetAddress.h"
#include "muduo/base/Mutex.h"
#include "TcpConnection.h"
#include "muduo/base/Atomic.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class Connector;
        class EventLoop;

        using ConnectorPtr = std::shared_ptr<Connector>;

        class TcpClient:noncopyable{
        public:
            TcpClient(EventLoop *loop, const InetAddress &serverAddr,
                      const std::string &nameArg);
            ~TcpClient();
            void connect();
   
            void disconnect();
        
            void stop();
            TcpConnectionPtr connection() const
            {
                MutexLockGuard lock(mutex_);
                return connection_;
            }
            EventLoop *getLoop() const { return loop_; }
            bool retry() const { return retry_.get() != 0; }
            void enableRetry() { retry_.set(1); }

            const std::string &name() const { return name_; }

            void setConnectionCallback(ConnectionCallback cb)
            {
                connectionCallback_ = std::move(cb);
            }

            void setMessageCallback(MessageCallback cb)
            {
                messageCallback_ = std::move(cb);
            }
    
            void setWriteCompleteCallback(WriteCompleteCallback cb)
            {
                writeCompleteCallback_ = std::move(cb);
            }

        private:
            void newConnection(int sockfd);

            void removeConnection(const TcpConnectionPtr &conn);

            EventLoop *loop_;
            ConnectorPtr connector_;
            const std::string name_;

            ConnectionCallback connectionCallback_;
            MessageCallback messageCallback_;
            WriteCompleteCallback writeCompleteCallback_;

            AtomicInt32 retry_{0};
            AtomicInt32 connect_{0};

            int nextConnId_ = 1;

            mutable MutexLock mutex_;
            TcpConnectionPtr connection_;
        };
    }
}