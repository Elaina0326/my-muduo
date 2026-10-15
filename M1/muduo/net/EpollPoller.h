#pragma once

#include<sys/epoll.h>

#include<vector>

#include "Poller.h"

namespace muduo{
    namespace net{
        class EpollPoller:public Poller{
            public:
                explicit EpollPoller(EventLoop *loop);
                ~EpollPoller() override;

                Timestamp poll(int timeoutMs, ChannelList *activeChannels) override;
                void updateChannel(Channel *channel) override;
                void removeChannel(Channel *channel) override;

            private:
                static const int KInitEventListSize = 16;

                void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
                void update(int operation, Channel *channel);
                const char *operationToString(int op) const;

                using EventList = std::vector<struct epoll_event>;
                int epollfd_;
                EventList events_;
        };
    }
}