#include "InetAddress.h"

#include <arpa/inet.h>
#include <cstring>

#include "Endian.h"
#include "SocketsOps.h"

namespace muduo{
    namespace net{
        static_assert(sizeof(InetAddress) == sizeof(struct sockaddr_in),
                      "InetAddress 必须和 sockaddr_in 一样大");
        static_assert(offsetof(struct sockaddr_in, sin_addr) == 4,
                      "sockaddr_in 地址字段偏移必须是 4");
        static_assert(offsetof(struct sockaddr_in, sin_port) == 2,
                      "sockaddr_in 端口字段偏移必须是 2");

        InetAddress::InetAddress(uint16_t port, bool loopbackOnly)
        {
            std::memset(&addr_, 0, sizeof(addr_));
            addr_.sin_family = AF_INET;
            in_addr_t ip = loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY;
            addr_.sin_addr.s_addr = hostToNetwork32(ip);
            addr_.sin_port = hostToNetwork16(port);
        }
        InetAddress::InetAddress(StringArg ip, uint16_t port)
        {
            std::memset(&addr_, 0, sizeof(addr_));
            sockets::fromIpPort(ip, port, &addr_);
        }
        std::string InetAddress::toIp() const
        {
            char buf[64] = {0};
            sockets::toIp(buf, sizeof(buf), &addr_);
            return buf;
        }
        std::string InetAddress::toIpPort() const
        {
            char buf[64] = {0};
            sockets::toIpPort(buf, sizeof(buf), &addr_);
            return buf;
        }
        uint16_t InetAddress::toPort() const
        {
            return networkToHost16(addr_.sin_port);
        }
        const struct sockaddr *InetAddress::getSockAddr() const
        {
            return sockets::sockaddr_cast(&addr_);
        }
    }
    
    
}