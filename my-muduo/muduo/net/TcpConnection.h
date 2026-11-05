#pragma once

#include<sys/types.h>

#include <any>
#include <memory>
#include <string>
#include <vector>

#include "Buffer.h"
#include "Callbacks.h"
#include "InetAddress.h" 
#include "TimerId.h"
#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class Channel;
        class EventLoop;
        class Socket;

        class TcpConnection:noncopyable,public std::enable_shared_from_this<TcpConnection>{
            public:
                TcpConnection(EventLoop *loop, const std::string &nameArg, int sockfd,
                              const InetAddress &localAddr, const InetAddress &peerAddr);
                ~TcpConnection();
                EventLoop *getLoop() const { return loop_; }
                const std::string &name() const { return name_; }
                const InetAddress &localAddress() const { return localAddr_; }
                const InetAddress &peerAddress() const { return peerAddr_; }

                bool connected() const { return state_ == kConnected; }
                bool disconnected() const { return state_ == kDisconnected; }

                void send(const std::string &message);
                void send(const void *message, int len);
                void send(Buffer *message);

                void sendMulti(const std::vector<std::string> &parts);
                void sendFile(int filefd, off_t offset, size_t count,
                              std::shared_ptr<void> guard = nullptr);

                void shutdown();   
                void forceClose(); 
                void forceCloseWithDelay(double seconds);

                void startRead();
                void stopRead();

                bool isReading() const { return reading_; }

                void setTcpNoDelay(bool on);

                void setContext(std::any ctx) { context_ = std::move(ctx); }
                const std::any &getContext() const { return context_; }
                std::any *getMutableContext() { return &context_; }

                void setConnectionCallback(const ConnectionCallback &cb)
                {
                    connectionCallback_ = cb;
                }
                void setMessageCallback(const MessageCallback &cb)
                {
                    messageCallback_ = cb;
                }
                void setWriteCompleteCallback(const WriteCompleteCallback &cb)
                {
                    writeCompleteCallback_ = cb;
                }
                void setHighWaterMarkCallback(const HighWaterMarkCallback &cb,
                                              size_t highWaterMark)
                {
                    highWaterMarkCallback_ = cb;
                    highWaterMark_ = highWaterMark;
                }
                void setCloseCallback(const CloseCallback &cb) { closeCallback_ = cb; }

                Buffer *inputBuffer() { return &inputBuffer_; }
                Buffer *outputBuffer() { return &outputBuffer_; }
                size_t outputBufferSize() const { return outputBuffer_.readableBytes(); }
                bool hasPendingData() const
                {
                    return sendFileActive_ || outputBuffer_.readableBytes() > 0;
                }

                void connectEstablished();
                void connectDestroyed();
                void enableIdleTimeout(int seconds);

            private:
                enum StateE
                {
                    kDisconnected,
                    kConnecting,
                    kConnected,
                    kDisconnecting
                };
                void handleRead(Timestamp receiveTime);
                void handleWrite();
                void handleClose();
                void handleError();

                void armIdleTimer(double delay);
                void handleIdleTimeout();

                void sendInLoop(const void *message, size_t len);
                void sendMultiInLoop(const std::vector<std::string> &parts);
                void notifyHighWaterMark(size_t added);
                void sendFileInLoop(int filefd, off_t offset, size_t count,
                                    std::shared_ptr<void> guard);
                void continueSendFile();
                void shutdownInLoop();
                void forceCloseInLoop();

                void setState(StateE s) { state_ = s; }
                const char *stateToString() const;
                void startReadInLoop();
                void stopReadInLoop();
                
                EventLoop *loop_;
                const std::string name_;

                StateE state_;
                bool reading_;

                std::unique_ptr<Socket> socket_;
                std::unique_ptr<Channel> channel_;
                const InetAddress localAddr_;
                const InetAddress peerAddr_;

                ConnectionCallback connectionCallback_;
                MessageCallback messageCallback_;
                WriteCompleteCallback writeCompleteCallback_;
                HighWaterMarkCallback highWaterMarkCallback_;
                CloseCallback closeCallback_;

                size_t highWaterMark_;
                Timestamp lastActiveTime_;
                TimerId idleTimer_;
                int idleTimeoutSeconds_ = 0;
                bool idleTimerArmed_ = false;

                Buffer inputBuffer_;
                Buffer outputBuffer_;
                std::any context_;

                int sendFileFd_ = -1;
                off_t sendFileOffset_ = 0;
                size_t sendFileRemaining_ = 0;
                bool sendFileActive_ = false;

                std::shared_ptr<void> sendFileGuard_;
        };
    }
}