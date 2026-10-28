#include "Callbacks.h"

#include "Buffer.h"
#include "TcpConnection.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        void defaultConnectionCallback(const TcpConnectionPtr &conn)
        {
            LOG_TRACE << conn->localAddress().toIpPort() << " -> "
                      << conn->peerAddress().toIpPort() << " is "
                      << (conn->connected() ? "UP" : "DOWN");
        }
        void defaultMessageCallback(const TcpConnectionPtr &conn, Buffer *buffer,
                                    Timestamp receiveTime)
        {
            (void)conn;
            (void)receiveTime;
            buffer->retrieveAll();
        }
    }
}