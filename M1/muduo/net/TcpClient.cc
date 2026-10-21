#include "TcpClient.h"

#include <cassert>
#include <cstdio>
#include <utility>

#include "Connector.h"
#include "EventLoop.h"
#include "SocketsOps.h"
#include "TcpConnection.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        namespace detail{
            void removeConnection(EventLoop *loop, const TcpConnectionPtr &conn)
            {
                loop->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
            }
            void removeConnector(const ConnectorPtr &connector)
            {
                (void)connector;
            }
        }
        TcpClient::TcpClient(EventLoop *loop, const InetAddress &serverAddr,
                             const std::string &nameArg)
            : loop_(loop),
              connector_(new Connector(loop, serverAddr)),
              name_(nameArg),
              connectionCallback_(defaultConnectionCallback),
              messageCallback_(defaultMessageCallback)
        {
            connector_->setNewConnectionCallback(
                std::bind(&TcpClient::newConnection, this, std::placeholders::_1));
            LOG_DEBUG << "TcpClient::TcpClient[" << name_ << "] - connector "
                      << connector_.get();
        }
        TcpClient::~TcpClient()
        {
            loop_->assertInLoopThread();

            LOG_DEBUG << "TcpClient::~TcpClient[" << name_ << "] - connector "
                      << connector_.get();

            TcpConnectionPtr conn;
            bool unique = false;
            {
                MutexLockGuard lock(mutex_);
                unique = (connection_.use_count() == 1);
                conn = connection_;
            }

            if (conn)
            {
                assert(loop_ == conn->getLoop());
                CloseCallback cb =
                    std::bind(&detail::removeConnection, loop_, std::placeholders::_1);
                loop_->runInLoop([conn, cb]
                                 { conn->setCloseCallback(cb); });

                if (unique)
                {
                    conn->forceClose();
                }
            }
            else
            {
                connector_->stop();
                loop_->runAfter(1.0, std::bind(&detail::removeConnector, connector_));
            }
        }
        void TcpClient::connect()
        {
            LOG_INFO << "TcpClient::connect[" << name_ << "] - connecting to "
                     << connector_->serverAddress().toIpPort();
            connect_.set(1);
            connector_->start();
        }
        void TcpClient::disconnect()
        {
            connect_.set(0);
            {
                MutexLockGuard lock(mutex_);
                if (connection_)
                {
                    connection_->shutdown();
                }
            }
        }
        void TcpClient::stop()
        {
            connect_.set(0);
            connector_->stop();
        }
        void TcpClient::newConnection(int sockfd)
        {
            loop_->assertInLoopThread();

            InetAddress peerAddr(sockets::getPeerAddr(sockfd));
            InetAddress localAddr(sockets::getLocalAddr(sockfd));

            char buf[32];
            std::snprintf(buf, sizeof(buf), ":%s#%d", peerAddr.toIpPort().c_str(),
                          nextConnId_);
            ++nextConnId_;

            std::string connName = name_ + buf;

            TcpConnectionPtr conn(
                new TcpConnection(loop_, connName, sockfd, localAddr, peerAddr));

            conn->setConnectionCallback(connectionCallback_);
            conn->setMessageCallback(messageCallback_);
            conn->setWriteCompleteCallback(writeCompleteCallback_);
            conn->setCloseCallback(
                std::bind(&TcpClient::removeConnection, this, std::placeholders::_1));

            {
                MutexLockGuard lock(mutex_);
                if (connection_ != nullptr)
                {
                    LOG_FATAL << "TcpClient::newConnection[" << name_
                              << "]: 上一个连接还没走完 removeConnection，"
                              << "拒绝建立新连接（先等 closeCallback 把 connection_ 置空）";
                }
                connection_ = conn;
            }

            conn->connectEstablished();
        }
        void TcpClient::removeConnection(const TcpConnectionPtr &conn)
        {
            loop_->assertInLoopThread();
            assert(loop_ == conn->getLoop());

            {
                MutexLockGuard lock(mutex_);
                assert(connection_ == conn);
                connection_.reset();
            }

            loop_->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));

            if (retry_.get() != 0 && connect_.get() != 0)
            {
                LOG_INFO << "TcpClient::removeConnection[" << name_ << "] - Reconnecting to "
                         << connector_->serverAddress().toIpPort();
                connector_->restart();
            }
        }
    }
}