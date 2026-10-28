#pragma once

#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class InetAddress;

        class Socket:noncopyable
        {
        public:
            explicit Socket(int sockfd) : sockfd_(sockfd) {}

            ~Socket();
            int fd() const { return sockfd_; }
            void bindAddress(const InetAddress &localaddr);

            void listen();
            int accept(InetAddress *peeraddr);
            void shutdownWrite();
            void setReuseAddr(bool on);
            
            void setTcpNoDelay(bool on);

            void setReusePort(bool on);

            void setKeepAlive(bool on);

        private:
            const int sockfd_;
        };
    }
}