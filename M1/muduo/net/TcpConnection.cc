#include "TcpConnection.h"

#include <sys/sendfile.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "SocketsOps.h"
#include "muduo/base/Logging.h"

namespace muduo
{
    namespace net
    {
        TcpConnection::TcpConnection(EventLoop *loop, const std::string &nameArg,
                                     int sockfd, const InetAddress &localAddr,
                                     const InetAddress &peerAddr)
            : loop_(loop),
              name_(nameArg),
              state_(kConnecting),
              reading_(true),
              socket_(new Socket(sockfd)),
              channel_(new Channel(loop, sockfd)),
              localAddr_(localAddr),
              peerAddr_(peerAddr),
              highWaterMark_(64 * 1024 * 1024)
        {
            assert(loop_ != nullptr);

            channel_->setReadCallback(
                std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
            channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
            channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
            channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));

            LOG_DEBUG << "TcpConnection::ctor[" << name_ << "] at " << this
                      << " fd=" << sockfd;
            socket_->setKeepAlive(true);
        }
        TcpConnection::~TcpConnection()
        {
            LOG_DEBUG << "TcpConnection::dtor[" << name_ << "] at " << this
                      << " fd=" << channel_->fd() << " state=" << stateToString();
            assert(state_ == kDisconnected);
        }
        void TcpConnection::handleRead(Timestamp receiveTime)
        {
            loop_->assertInLoopThread();
            int savedErrno = 0;

            ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

            if (n > 0)
            {
                messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
            }
            else if (n == 0)
            {
                handleClose();
            }
            else
            {
                errno = savedErrno;
                LOG_SYSERR << "TcpConnection::handleRead";
                handleError();
            }
        }
        void TcpConnection::handleWrite()
        {
            loop_->assertInLoopThread();

            if (sendFileActive_)
            {
                continueSendFile();
                return;
            }

            if (channel_->isWriting())
            {
                int savedErrno = 0;
                ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);

                if (n > 0)
                {
                    outputBuffer_.retrieve(static_cast<size_t>(n));

                    if (outputBuffer_.readableBytes() == 0)
                    {

                        channel_->disableWriting();

                        if (writeCompleteCallback_)
                        {
                            loop_->queueInLoop(std::bind(writeCompleteCallback_,
                                                         shared_from_this()));
                        }

                        if (state_ == kDisconnecting)
                        {
                            shutdownInLoop();
                        }
                    }
                }
                else
                {
                    errno = savedErrno;
                    LOG_SYSERR << "TcpConnection::handleWrite";
                }
            }
            else
            {
                LOG_TRACE << "Connection fd = " << channel_->fd()
                          << " is down, no more writing";
            }
        }
        void TcpConnection::handleClose()
        {
            loop_->assertInLoopThread();
            LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();

            assert(state_ == kConnected || state_ == kDisconnecting);

            setState(kDisconnected);
            channel_->disableAll();

            TcpConnectionPtr guardThis(shared_from_this());

            connectionCallback_(guardThis);

            closeCallback_(guardThis);
        }
        void TcpConnection::handleError()
        {
            int err = sockets::getSocketError(channel_->fd());
            LOG_ERROR << "TcpConnection::handleError [" << name_
                      << "] - SO_ERROR = " << err << " " << std::strerror(err);
        }
        void TcpConnection::send(const std::string &message)
        {
            if (loop_->isInLoopThread())
            {
                if (state_ == kConnected)
                {
                    sendInLoop(message.data(), message.size());
                }
                return;
            }
            std::string copied = message;
            loop_->runInLoop([self = shared_from_this(), copied]
                             {
        if (self->state_ == kConnected) {
            self->sendInLoop(copied.data(), copied.size());
        } });
        }
        void TcpConnection::send(const void *data, int len)
        {
            send(std::string(static_cast<const char *>(data),
                             static_cast<size_t>(len)));
        }
        void TcpConnection::send(Buffer *buf)
        {
            if (buf == nullptr)
            {
                return;
            }

            if (loop_->isInLoopThread())
            {
                if (state_ == kConnected)
                {
                    sendInLoop(buf->peek(), buf->readableBytes());
                    buf->retrieveAll();
                }
                return;
            }

            std::string copied = buf->retrieveAllAsString();
            loop_->runInLoop([self = shared_from_this(), copied]
                             {
        if (self->state_ == kConnected) {
            self->sendInLoop(copied.data(), copied.size());
        } });
        }
        void TcpConnection::sendInLoop(const void *data, size_t len)
        {
            loop_->assertInLoopThread();
            ssize_t nwrote = 0;
            size_t remaining = len;
            bool faultError = false;

            if (state_ == kDisconnected)
            {
                LOG_WARN << "disconnected, give up writing";
                return;
            }

            if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
            {
                nwrote = sockets::write(channel_->fd(), data, len);

                if (nwrote >= 0)
                {
                    remaining = len - static_cast<size_t>(nwrote);

                    if (remaining == 0 && writeCompleteCallback_)
                    {
                        loop_->queueInLoop(std::bind(writeCompleteCallback_,
                                                     shared_from_this()));
                    }
                }
                else
                {
                    nwrote = 0;
                    if (errno != EWOULDBLOCK)
                    {
                        LOG_SYSERR << "TcpConnection::sendInLoop";
                        if (errno == EPIPE || errno == ECONNRESET)
                        {
                            faultError = true;
                        }
                    }
                }
            }

            assert(remaining <= len);
            if (!faultError && remaining > 0)
            {
                size_t oldLen = outputBuffer_.readableBytes();

                if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ &&
                    highWaterMarkCallback_)
                {
                    loop_->queueInLoop(std::bind(highWaterMarkCallback_,
                                                 shared_from_this(),
                                                 oldLen + remaining));
                }

                outputBuffer_.append(static_cast<const char *>(data) + nwrote, remaining);

                if (!channel_->isWriting())
                {
                    channel_->enableWriting();
                }
            }
        }
        void TcpConnection::sendMulti(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return;
            }

            if (loop_->isInLoopThread())
            {
                if (state_ == kConnected)
                {
                    sendMultiInLoop(parts);
                }
                return;
            }

            std::vector<std::string> copied(parts);
            loop_->runInLoop([self = shared_from_this(), copied]
                             {
        if (self->state_ == kConnected) {
            self->sendMultiInLoop(copied);
        } });
        }
        void TcpConnection::sendMultiInLoop(const std::vector<std::string> &parts)
        {
            loop_->assertInLoopThread();

            if (state_ == kDisconnected)
            {
                return;
            }

            if (channel_->isWriting() || outputBuffer_.readableBytes() > 0)
            {
                for (const auto &p : parts)
                {
                    outputBuffer_.append(p.data(), p.size());
                }
                if (!channel_->isWriting())
                {
                    channel_->enableWriting();
                }
                return;
            }

            std::vector<struct iovec> vec;
            vec.reserve(parts.size());
            size_t total = 0;
            for (const auto &p : parts)
            {
                if (p.empty())
                {
                    continue;
                }
                struct iovec iov;
                iov.iov_base = const_cast<char *>(p.data());
                iov.iov_len = p.size();
                vec.push_back(iov);
                total += p.size();
            }

            if (vec.empty())
            {
                return;
            }

            ssize_t nwrote = ::writev(channel_->fd(), vec.data(),
                                      static_cast<int>(vec.size()));

            if (nwrote >= 0)
            {
                if (static_cast<size_t>(nwrote) == total)
                {
                    if (writeCompleteCallback_)
                    {
                        loop_->queueInLoop(std::bind(writeCompleteCallback_,
                                                     shared_from_this()));
                    }
                    return;
                }

                size_t skip = static_cast<size_t>(nwrote);
                for (const auto &p : parts)
                {
                    if (p.empty())
                    {
                        continue;
                    }
                    if (skip >= p.size())
                    {
                        skip -= p.size();
                    }
                    else
                    {
                        outputBuffer_.append(p.data() + skip, p.size() - skip);
                        skip = 0;
                    }
                }
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                for (const auto &p : parts)
                {
                    outputBuffer_.append(p.data(), p.size());
                }
            }
            else if (errno == EINTR)
            {
                sendMultiInLoop(parts);
                return;
            }
            else
            {
                LOG_SYSERR << "TcpConnection::sendMultiInLoop";
                if (errno == EPIPE || errno == ECONNRESET)
                {
                    return;
                }
            }

            if (!channel_->isWriting())
            {
                channel_->enableWriting();
            }
        }
        void TcpConnection::sendFile(int filefd, off_t offset, size_t count,
                                     std::shared_ptr<void> guard)
        {
            if (filefd < 0 || count == 0)
            {
                return;
            }

            if (loop_->isInLoopThread())
            {
                if (state_ == kConnected)
                {
                    sendFileInLoop(filefd, offset, count, std::move(guard));
                }
                return;
            }

            loop_->runInLoop([self = shared_from_this(), filefd, offset, count,
                              guard]() mutable
                             {
        if (self->state_ == kConnected) {
            self->sendFileInLoop(filefd, offset, count, std::move(guard));
        } });
        }
        void TcpConnection::sendFileInLoop(int filefd, off_t offset, size_t count,
                                           std::shared_ptr<void> guard)
        {
            loop_->assertInLoopThread();

            if (state_ == kDisconnected)
            {
                return;
            }
            if (sendFileActive_)
            {
                LOG_ERROR << "TcpConnection::sendFile 重入：上一次文件还没发完，"
                          << "本次调用被忽略（已发 " << sendFileRemaining_ << " 字节未完成）";
                return;
            }
            sendFileGuard_ = std::move(guard);

            if (channel_->isWriting() || outputBuffer_.readableBytes() > 0)
            {
                LOG_WARN << "sendFile: 发送缓冲非空，降级为 read + send";

                std::string content;
                content.resize(count);

                ssize_t n = ::pread(filefd, &content[0], count, offset);
                if (n > 0)
                {
                    content.resize(static_cast<size_t>(n));
                    outputBuffer_.append(content);
                    if (!channel_->isWriting())
                    {
                        channel_->enableWriting();
                    }
                }
                else if (n < 0)
                {
                    LOG_SYSERR << "sendFile fallback pread";
                }

                sendFileGuard_.reset();
                return;
            }

            sendFileFd_ = filefd;
            sendFileOffset_ = offset;
            sendFileRemaining_ = count;
            sendFileActive_ = true;

            continueSendFile();
        }
        void TcpConnection::continueSendFile()
        {
            loop_->assertInLoopThread();

            while (sendFileRemaining_ > 0)
            {
                ssize_t n = ::sendfile(channel_->fd(), sendFileFd_, &sendFileOffset_,
                                       sendFileRemaining_);

                if (n > 0)
                {
                    sendFileRemaining_ -= static_cast<size_t>(n);
                }
                else if (n < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        if (!channel_->isWriting())
                        {
                            channel_->enableWriting();
                        }
                        return;
                    }
                    if (errno == EINTR)
                    {
                        continue;
                    }

                    LOG_SYSERR << "TcpConnection::continueSendFile";
                    sendFileActive_ = false;
                    sendFileRemaining_ = 0;
                    sendFileGuard_.reset(); 
                    forceClose();
                    return;
                }
                else
                {
                    break;
                }
            }

            sendFileActive_ = false;
            sendFileRemaining_ = 0;
            sendFileGuard_.reset();

            if (channel_->isWriting())
            {
                channel_->disableWriting();
            }

            if (writeCompleteCallback_)
            {
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }

            if (state_ == kDisconnecting)
            {
                shutdownInLoop();
            }
        }
        void TcpConnection::shutdown()
        {
            if (loop_->isInLoopThread())
            {
                if (state_ == kConnected)
                {
                    setState(kDisconnecting);
                    shutdownInLoop();
                }
                return;
            }

            loop_->runInLoop([self = shared_from_this()]
                             {
        if (self->state_ == kConnected) {
            self->setState(kDisconnecting);
            self->shutdownInLoop();
        } });
        }
        void TcpConnection::shutdownInLoop(){
            loop_->assertInLoopThread();
            if (!channel_->isWriting()){
                socket_->shutdownWrite();
            }
        }
        void TcpConnection::forceClose()
        {
            if (loop_->isInLoopThread())
            {
                forceCloseInLoop();
                return;
            }

            loop_->queueInLoop(
                [self = shared_from_this()]
                { self->forceCloseInLoop(); });
        }
        void TcpConnection::forceCloseWithDelay(double seconds)
        {
            loop_->runInLoop([self = shared_from_this(), seconds]
                             {
        if (self->state_ == kConnected ||
            self->state_ == kDisconnecting) {
            self->setState(kDisconnecting);
            LOG_INFO << "forceCloseWithDelay: " << seconds << " 秒后关闭";
            self->loop_->runAfter(seconds, [self] { self->forceClose(); });
        } });
        }
        void TcpConnection::forceCloseInLoop()
        {
            loop_->assertInLoopThread();
            if (state_ == kConnected || state_ == kDisconnecting)
            {
                handleClose();
            }
        }
        void TcpConnection::setTcpNoDelay(bool on)
        {
            socket_->setTcpNoDelay(on);
        }
        void TcpConnection::startRead()
        {
            if (loop_->isInLoopThread())
            {
                startReadInLoop();
                return;
            }
            loop_->runInLoop(
                [self = shared_from_this()]
                { self->startReadInLoop(); });
        }
        void TcpConnection::startReadInLoop()
        {
            loop_->assertInLoopThread();
            if (!reading_ || !channel_->isReading())
            {
                channel_->enableReading();
                reading_ = true;
            }
        }
        void TcpConnection::stopRead()
        {
            if (loop_->isInLoopThread())
            {
                stopReadInLoop();
                return;
            }
            loop_->runInLoop([self = shared_from_this()]
                             { self->stopReadInLoop(); });
        }
        void TcpConnection::stopReadInLoop()
        {
            loop_->assertInLoopThread();
            if (reading_ || channel_->isReading())
            {
                channel_->disableReading();
                reading_ = false;
            }
        }
        void TcpConnection::connectEstablished()
        {
            loop_->assertInLoopThread();
            assert(state_ == kConnecting);

            setState(kConnected);
            channel_->tie(shared_from_this());

            channel_->enableReading();

            connectionCallback_(shared_from_this());
        }
        void TcpConnection::connectDestroyed()
        {
            loop_->assertInLoopThread();

            if (state_ == kConnected)
            {
                setState(kDisconnected);
                channel_->disableAll();
                connectionCallback_(shared_from_this());
            }
            channel_->remove();
        }
        const char *TcpConnection::stateToString() const
        {
            switch (state_)
            {
            case kDisconnected:
                return "kDisconnected";
            case kConnecting:
                return "kConnecting";
            case kConnected:
                return "kConnected";
            case kDisconnecting:
                return "kDisconnecting";
            default:
                return "unknown state";
            }
        }
    }
}