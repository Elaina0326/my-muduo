#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <string>

#include "muduo/base/Types.h"

namespace muduo{
    namespace net{
        class InetAddress{
        public:
            explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);
            InetAddress(StringArg ip, uint16_t port);
            explicit InetAddress(const struct sockaddr_in &addr) : addr_(addr) {}
            sa_family_t family() const { return addr_.sin_family; }

            std::string toIp() const;
            std::string toIpPort() const;
            uint16_t toPort() const;

            const struct sockaddr *getSockAddr() const;

            uint32_t ipNetEndian() const { return addr_.sin_addr.s_addr; }
            uint16_t portNetEndian() const { return addr_.sin_port; }

            void setSockAddrInet(const struct sockaddr_in &addr) { addr_ = addr; }

        private:
            struct sockaddr_in addr_;
        };
    }
}