#pragma once

#include<map>
#include<vector>

#include "muduo/base/Timestamp.h"
#include "muduo/base/noncopyable.h"

namespace muduo{
    namespace net{
        class Channel;
        class EventLoop;

        class Poller:noncopyable{
            public:
                using ChannelList = std::vector<Channel *>;
                explicit Poller(EventLoop* loop):ownerLoop_(loop){}
                virtual ~Poller() = default;

                virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels)=0 ;

                virtual void updateChannel(Channel *CHANNEL) = 0;

                virtual void removeChannel(Channel *channel) = 0;
                virtual bool hasChannel(Channel *Channel) const;

                static Poller *newDefaultPoller(EventLoop *loop);

                void assertInLoopThread()const;

            protected:
                using ChannelMap = std::map<int, Channel *>;
                ChannelMap channels_;

            private:
                EventLoop *ownerLoop_;
        };
    }
}