#pragma once

#include<arpa/inet.h>
#include<sys/types.h>

#include<cstdint>

namespace muduo{
    namespace net{
        namespace sockets{
            int createNonblockingOrDie();
            int connect(int sockfd, const struct sockaddr *addr);
            void bindOrDie(int sockfd, const struct sockaddr *addr);
            void listenOrDie(int sockfd);
            int accept(int sockfd, struct sockaddr_in *addr);

            ssize_t read(int sockfd, void *buf, size_t count);
            ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt);

            ssize_t write(int sockfd, const void *buf, size_t count);
            void close(int sockfd);
            void shutdownWrite(int sockfd);
            void toIpPort(char *buf, size_t size, const struct sockaddr_in *addr);
            void toIp(char *buf, size_t size, const struct sockaddr_in *addr);
            void fromIpPort(const char *ip, uint16_t port, struct sockaddr_in *addr);

            int getSocketError(int sockfd);
            const struct sockaddr *sockaddr_cast(const struct sockaddr_in *addr);
            struct sockaddr *sockaddr_cast(struct sockaddr_in *addr);
            struct sockaddr_in getLocalAddr(int sockfd);
            struct sockaddr_in getPeerAddr(int sockfd);

            bool isSelfConnect(int sockfd);
        }
    }
}