#include "EpollPoller.h"

#include<unistd.h>

#include<cstring>
#include<cerrno>

#include"Channel.h"
#include"EventLoop.h"
#include "muduo/base/Logging.h"

namespace muduo{
    namespace net{
        const int kNew = -1;
        const int kAdded = 1;
        const int kDeleted = 2;
        EpollPoller::EpollPoller(EventLoop *loop):Poller(loop),epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
        events_(KInitEventListSize){
            if(epollfd_<0){
                LOG_SYSERR << "EpollPoller:EpollPoller";
                abort();
            }
        }
        EpollPoller::~EpollPoller(){
            ::close(epollfd_);
        }
        Timestamp EpollPoller::poll(int timeoutMs,ChannelList* activeChannels){
            int numEvents = ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
            int savedErrno = errno;
            Timestamp now(Timestamp::now());
            if(numEvents>0){
                fillActiveChannels(numEvents, activeChannels);
                if(static_cast<size_t>(numEvents)==events_.size()){
                    events_.resize(events_.size() * 2);
                }
            }
            else if (numEvents == 0)
            {
            }
            else
            {
                if (savedErrno != EINTR)
                {
                    errno = savedErrno;
                    LOG_SYSERR << "EpollPoller::poll()";
                }
            }
            return now;
        }
        void EpollPoller::fillActiveChannels(int numEvents,ChannelList* activeChannels)const{
            for (int i = 0; i < numEvents;++i){
                const size_t idx = static_cast<size_t>(i);
                Channel *channel = static_cast<Channel *>(events_[idx].data.ptr);
                channel->set_revents(events_[idx].events);
                activeChannels->push_back(channel);
            }
        }
        void EpollPoller::updateChannel(Channel*channel){
            const int idx = channel->index();
            int fd = channel->fd() ;
            if(idx==kNew||idx==kDeleted){
                if(idx==kNew){
                    channels_[fd] = channel;
                }
                channel->set_index(kAdded);
                update(EPOLL_CTL_ADD, channel);
            }else{
                if(channel->isNoneEvent()){
                    update(EPOLL_CTL_DEL, channel);
                    channel->set_index(kDeleted);
                }else{
                    update(EPOLL_CTL_MOD, channel);
                }
            }
        }

        void EpollPoller::removeChannel(Channel* channel){
            int fd = channel->fd();
            channels_.erase(fd);
            int idx = channel->index();
            if(idx==kAdded){
                update(EPOLL_CTL_DEL, channel);
            }
            channel->set_index(kNew);
        }

        void EpollPoller::update(int operation, Channel *channel){
            struct epoll_event event;
            std::memset(&event, 0, sizeof(event));

            event.events = static_cast<uint32_t>(channel->events());
            event.data.ptr = channel;
            int fd = channel->fd();
            if(::epoll_ctl(epollfd_,operation,fd,&event)<0){
                if(operation==EPOLL_CTL_DEL){
                    LOG_SYSERR << "epoll_ctl op=DEL";
                }else{
                    LOG_SYSERR << "epoll_ctl op = " << operationToString(operation) << " fd = " << fd;
                    abort();
                }
            }
        }

        const char* EpollPoller::operationToString(int op)const{
            switch(op){
                case EPOLL_CTL_ADD :
                    return "ADD";
                case EPOLL_CTL_DEL:
                    return "DEL";
                case EPOLL_CTL_MOD:
                    return "MOD";
                default:
                    return "UNKNOWN";
                }
        }
    }
}
