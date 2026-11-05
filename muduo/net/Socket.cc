#include "Socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "InetAddress.h"
#include "SocketsOps.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        Socket::~Socket()
        {
            sockets::close(sockfd_);
        }
        void Socket::bindAddress(const InetAddress &addr)
        {
            sockets::bindOrDie(sockfd_, addr.getSockAddr());
        }

        void Socket::listen()
        {
            sockets::listenOrDie(sockfd_);
        }
        int Socket::accept(InetAddress *peeraddr)
        {
            struct sockaddr_in addr;
            int connfd = sockets::accept(sockfd_, &addr);
            if (connfd >= 0)
            {
                peeraddr->setSockAddrInet(addr);
            }
            return connfd;
        }
        void Socket::shutdownWrite()
        {
            sockets::shutdownWrite(sockfd_);
        }
        void Socket::setReuseAddr(bool on)
        {
            int optval = on ? 1 : 0;
            ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        }

        void Socket::setReusePort(bool on)
        {
            int optval = on ? 1 : 0;
            int ret = ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval,
                                   sizeof(optval));
            if (ret < 0 && on)
            {
                LOG_SYSERR << "SO_REUSEPORT failed.";
            }
        }
        void Socket::setTcpNoDelay(bool on)
        {
            int optval = on ? 1 : 0;
            ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
        }

        void Socket::setKeepAlive(bool on)
        {
            int optval = on ? 1 : 0;
            ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
        }
    }
}