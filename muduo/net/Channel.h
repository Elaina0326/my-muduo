#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo
{
    namespace net
    {
        class EventLoop;
        class Channel : noncopyable
        {
        public:
            using EventCallback = std::function<void()>;
            using ReadEventCallback = std::function<void(Timestamp)>;

            Channel(EventLoop *loop, int fd);
            ~Channel();

            void handleEvent(Timestamp receiveTime);

            void setReadCallback(ReadEventCallback cb)
            {
                readCallback_ = std::move(cb);
            }
            void setWriteCallback(EventCallback cb)
            {
                writeCallback_ = std::move(cb);
            }
            void setCloseCallback(EventCallback cb)
            {
                closeCallback_ = std::move(cb);
            }
            void setErrorCallback(EventCallback cb)
            {
                errorCallback_ = std::move(cb);
            }

            void tie(const std::shared_ptr<void> &);

            int fd() const { return fd_; }

            uint32_t events() const { return events_; }
            void set_revents(uint32_t revt)
            {
                revents_ = revt;
            }
            bool isNoneEvent() const
            {
                return events_ == kNoneEvent;
            }

            void enableReading()
            {
                events_ |= kReadEvent;
                update();
            }

            void disableReading()
            {
                events_ &= ~kReadEvent;
                update();
            }
            void enableWriting()
            {
                events_ |= kWriteEvent;
                update();
            }

            void disableWriting()
            {
                events_ &= ~kWriteEvent;
                update();
            }

            void disableAll()
            {
                events_ = kNoneEvent;
                update();
            }
            bool isReading() const
            {
                return (events_ & kReadEvent) != 0;
            }

            bool isWriting() const
            {
                return (events_ & kWriteEvent) != 0;
            }
            int index() const
            {
                return index_;
            }
            void set_index(int idx)
            {
                index_ = idx;
            }

            EventLoop *ownerLoop()
            {
                return loop_;
            }

            void remove();

        private:
            void update();

            void handleEventWithGuard(Timestamp receiveTime);

            static const uint32_t kNoneEvent;
            static const uint32_t kReadEvent;
            static const uint32_t kWriteEvent;

            EventLoop *loop_;
            const int fd_;
            uint32_t events_;
            uint32_t revents_;
            int index_;

            std::weak_ptr<void> tie_;
            bool tied_ = false;

            bool eventHandling_ = false;

            ReadEventCallback readCallback_;
            EventCallback writeCallback_;
            EventCallback closeCallback_;
            EventCallback errorCallback_;
        };
    }
}