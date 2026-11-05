#include "Poller.h"

#include "Channel.h"
#include "EpollPoller.h"
#include "EventLoop.h"

namespace muduo{
    namespace net{
        Poller* Poller::newDefaultPoller(EventLoop*loop){
            return new EpollPoller(loop);
        }
        void Poller::assertInLoopThread()const{
            ownerLoop_->assertInLoopThread();
        }

        bool Poller::hasChannel(Channel *channel)const{
            auto it = channels_.find(channel->fd() );
            return it!=channels_.end()&&it->second==channel;
        }
    }
}