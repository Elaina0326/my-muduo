#include "TcpServer.h"

#include <cassert>
#include <cstdio>

#include "Acceptor.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "SocketsOps.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        TcpServer::TcpServer(EventLoop *loop, const InetAddress &listenAddr,
                             const std::string &nameArg, Option option):
                             loop_(loop),ipPort_(listenAddr.toIpPort()),
                             name_(nameArg),acceptor_(new Acceptor(loop,listenAddr,option==kReusePort)),threadPool_(new
                            EventLoopThreadPool(loop,name_) ),
                             connectionCallback_(defaultConnectionCallback),
      messageCallback_(defaultMessageCallback),
      started_(0){
            acceptor_->setNewConnectionCallback(
                std::bind(&TcpServer::newConnection, this, std::placeholders::_1,
                          std::placeholders::_2));
      }
      TcpServer::~TcpServer(){
          loop_->assertInLoopThread();
          for(auto &item:connections_){
              TcpConnectionPtr conn(item.second);
              item.second.reset();
              conn->getLoop()->runInLoopSync(
                  std::bind(&TcpConnection::connectDestroyed, conn));
          }
      }
      void TcpServer::setThreadNum(int numThreads)
      {
          assert(0 <= numThreads);
          threadPool_->setThreadNum(numThreads);
      }
      void TcpServer::start()
      {
          if (started_.getAndSet(1) == 0)
          {
              threadPool_->start(threadInitCallback_);
              assert(!acceptor_->listenning());
              loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
          }
      }
      void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
      {
          loop_->assertInLoopThread();

          EventLoop *ioLoop = threadPool_->getNextLoop();

          char buf[64];
          std::snprintf(buf, sizeof(buf), "-%s#%d", ipPort_.c_str(), nextConnId_);
          ++nextConnId_;
          std::string connName = name_ + buf;

          LOG_INFO << "TcpServer::newConnection [" << name_ << "] - new connection ["
                   << connName << "] from " << peerAddr.toIpPort();

          InetAddress localAddr(sockets::getLocalAddr(sockfd));
          TcpConnectionPtr conn(
              new TcpConnection(ioLoop, connName, sockfd, localAddr, peerAddr));

          connections_[connName] = conn; 

          conn->setConnectionCallback(connectionCallback_);
          conn->setMessageCallback(messageCallback_);
          conn->setWriteCompleteCallback(writeCompleteCallback_);
          conn->setCloseCallback(
              std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));
          ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
      }
      void TcpServer::removeConnection(const TcpConnectionPtr &conn)
      {
          loop_->runInLoop(
              std::bind(&TcpServer::removeConnectionInLoop, this, conn));
      }
      void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
      {
          loop_->assertInLoopThread();

          LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
                   << "] - connection " << conn->name();
          size_t n = connections_.erase(conn->name());
          assert(n == 1);
          (void)n;

          EventLoop *ioLoop = conn->getLoop();

          ioLoop->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
      }
    }
}