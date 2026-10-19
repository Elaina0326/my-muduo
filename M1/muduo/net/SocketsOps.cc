#include "SocketsOps.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cerrno>

#include "Endian.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        namespace sockets{
            int createNonblockingOrDie()
            {
                int sockfd = ::socket(AF_INET,
                                      SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                      IPPROTO_TCP);
                if (sockfd < 0)
                {
                    LOG_SYSERR << "sockets::createNonblockingOrDie";
                    abort();
                }
                return sockfd;
            }
            int connect(int sockfd, const struct sockaddr *addr)
            {
                return ::connect(sockfd, addr, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
            }
            void bindOrDie(int sockfd, const struct sockaddr *addr)
            {
                int ret = ::bind(sockfd, addr,
                                 static_cast<socklen_t>(sizeof(struct sockaddr_in)));
                if (ret < 0)
                {
                    LOG_SYSERR << "sockets::bindOrDie";
                    abort();
                }
            }
            void listenOrDie(int sockfd)
            {
                int ret = ::listen(sockfd, SOMAXCONN);
                if (ret < 0)
                {
                    LOG_SYSERR << "sockets::listenOrDie";
                    abort();
                }
            }
            int accept(int sockfd, struct sockaddr_in *addr)
            {
                socklen_t addrlen = static_cast<socklen_t>(sizeof(*addr));
                int connfd = ::accept4(sockfd, sockaddr_cast(addr), &addrlen,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (connfd < 0)
                {
                    int savedErrno = errno;
                    LOG_SYSERR << "Socket::accept";
                    switch (savedErrno)
                    {
                    // 这些错误是可恢复的，调用方可以继续 accept
                    case EAGAIN:
                    case ECONNABORTED:
                    case EINTR:
                    case EPROTO:
                    case EPERM:
                    case EMFILE: // fd 耗尽 —— 由 Acceptor 专门处理
                        errno = savedErrno;
                        break;
                    case EBADF:
                    case EFAULT:
                    case EINVAL:
                    case ENFILE:
                    case ENOBUFS:
                    case ENOMEM:
                    case ENOTSOCK:
                        LOG_FATAL << "unexpected error of ::accept " << savedErrno;
                        break;
                    default:
                        LOG_FATAL << "unknown error of ::accept " << savedErrno;
                        break;
                    }
                }
                return connfd;
            }
            ssize_t read(int sockfd, void *buf, size_t count)
            {
                return ::read(sockfd, buf, count);
            }

            ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt)
            {
                return ::readv(sockfd, iov, iovcnt);
            }

            ssize_t write(int sockfd, const void *buf, size_t count)
            {
                return ::write(sockfd, buf, count);
            }
            void close(int sockfd)
            {
                if (::close(sockfd) < 0)
                {
                    LOG_SYSERR << "sockets::close";
                }
            }

            void shutdownWrite(int sockfd)
            {
                if (::shutdown(sockfd, SHUT_WR) < 0)
                {
                    LOG_SYSERR << "sockets::shutdownWrite";
                }
            }
            void toIpPort(char *buf, size_t size, const struct sockaddr_in *addr)
            {
                toIp(buf, size, addr);
                size_t end = ::strlen(buf);
                uint16_t port = networkToHost16(addr->sin_port);
                std::snprintf(buf + end, size - end, ":%u", port);
            }
            void toIp(char *buf, size_t size, const struct sockaddr_in *addr)
            {
                ::inet_ntop(AF_INET, &addr->sin_addr, buf, static_cast<socklen_t>(size));
            }
            void fromIpPort(const char *ip, uint16_t port, struct sockaddr_in *addr)
            {
                addr->sin_family = AF_INET;
                addr->sin_port = hostToNetwork16(port);
                if (::inet_pton(AF_INET, ip, &addr->sin_addr) <= 0)
                {
                    LOG_SYSERR << "sockets::fromIpPort";
                }
            }
            int getSocketError(int sockfd)
            {
                int optval;
                socklen_t optlen = static_cast<socklen_t>(sizeof(optval));
                if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
                {
                    return errno;
                }
                return optval;
            }
            const struct sockaddr *sockaddr_cast(const struct sockaddr_in *addr)
            {
                return static_cast<const struct sockaddr *>(
                    static_cast<const void *>(addr));
            }
            struct sockaddr *sockaddr_cast(struct sockaddr_in *addr)
            {
                return static_cast<struct sockaddr *>(static_cast<void *>(addr));
            }
            struct sockaddr_in getPeerAddr(int sockfd)
            {
                struct sockaddr_in peeraddr;
                std::memset(&peeraddr, 0, sizeof(peeraddr));
                socklen_t addrlen = static_cast<socklen_t>(sizeof(peeraddr));
                if (::getpeername(sockfd, sockaddr_cast(&peeraddr), &addrlen) < 0)
                {
                    LOG_SYSERR << "sockets::getPeerAddr";
                }
                return peeraddr;
            }
            struct sockaddr_in getLocalAddr(int sockfd)
            {
                struct sockaddr_in localaddr;
                std::memset(&localaddr, 0, sizeof(localaddr));
                socklen_t addrlen = static_cast<socklen_t>(sizeof(localaddr));
                if (::getsockname(sockfd, sockaddr_cast(&localaddr), &addrlen) < 0)
                {
                    LOG_SYSERR << "sockets::getLocalAddr";
                }
                return localaddr;
            }
            bool isSelfConnect(int sockfd)
            {
                struct sockaddr_in localaddr = getLocalAddr(sockfd);
                struct sockaddr_in peeraddr = getPeerAddr(sockfd);
                return localaddr.sin_port == peeraddr.sin_port &&
                       localaddr.sin_addr.s_addr == peeraddr.sin_addr.s_addr;
            }
        }
    }
}