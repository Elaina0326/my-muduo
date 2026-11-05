#pragma once

#include <functional>

#include "Channel.h"
#include "Socket.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class EventLoop;
        class InetAddress;

        class Acceptor:noncopyable{
        public:
            using NewConnectionCallback =
                std::function<void(int sockfd, const InetAddress &)>;
            Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport);
            ~Acceptor();
            void setNewConnectionCallback(const NewConnectionCallback &cb)
            {
                newConnectionCallback_ = cb;
            }
            bool listenning() const { return listenning_; }

            // 给测试用：绑端口 0 时，通过它 + getsockname 拿实际端口
            int listenFd() const { return acceptSocket_.fd(); }

            void listen();

        private:
            void handleRead();

            EventLoop *loop_;
            Socket acceptSocket_;
            Channel acceptChannel_;
            NewConnectionCallback newConnectionCallback_;
            bool listenning_ = false;

            int idleFd_;
        };
    }
}