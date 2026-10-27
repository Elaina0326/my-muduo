#include "Channel.h"

#include <poll.h>
#include <sys/epoll.h>

#include <cassert>

#include "EventLoop.h"
#include "muduo/base/Logging.h"

namespace muduo
{
    namespace net
    {
        const uint32_t Channel::kNoneEvent = 0;
        const uint32_t Channel::kReadEvent = EPOLLIN ;
        const uint32_t Channel::kWriteEvent = EPOLLOUT;

        Channel::Channel(EventLoop *loop, int fd) : loop_(loop), fd_(fd), events_(0), revents_(0), index_(-1) {}

        Channel::~Channel()
        {
            assert(!eventHandling_);
        }

        void Channel::tie(const std::shared_ptr<void> &obj)
        {
            tie_ = obj;
            tied_ = true;
        }
        void Channel::update()
        {
            loop_->updateChannel(this);
        }
        void Channel::remove()
        {
            loop_->removeChannel(this);
        }
        void Channel::handleEvent(Timestamp receiveTime)
        {
            eventHandling_ = true;
            std::shared_ptr<void> guard;
            if (tied_)
            {
                guard = tie_.lock();
                if (guard)
                {
                    handleEventWithGuard(receiveTime);
                }
            }
            else
            {
                handleEventWithGuard(receiveTime);
            }
            eventHandling_ = false;
        }

        void Channel::handleEventWithGuard(Timestamp receiveTime)
        {
            if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
            {
                if (closeCallback_)
                {
                    closeCallback_();
                }
            }
            if (revents_ & EPOLLERR)
            {
                if (errorCallback_)
                {
                    errorCallback_();
                }
            }

            if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP))
            {
                if (readCallback_)
                {
                    readCallback_(receiveTime);
                }
            }
            if (revents_ & EPOLLOUT)
            {
                if (writeCallback_)
                {
                    writeCallback_();
                }
            }
        }
    }
}
