#include "EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>

#include "Channel.h"
#include "Poller.h"
#include "TimerId.h"
#include "TimerQueue.h"
#include "muduo/base/Condition.h"
#include "muduo/base/Logging.h"

namespace muduo
{
    namespace net
    {
        namespace
        {
            __thread EventLoop *t_loopInThisThread = nullptr;

            const int kPollTimeMs = 10000;
            int createEventfd()
            {
                int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                if (evtfd < 0)
                {
                    LOG_SYSERR << "failed in eventfd";
                    abort();
                }
                return evtfd;
            }
        }
        EventLoop *EventLoop::getEventLoopOfCurrentThread()
        {
            return t_loopInThisThread;
        }
        EventLoop::EventLoop() : threadId_(CurrentThread::tid()),
                                 poller_(Poller::newDefaultPoller(this)),
                                 timerQueue_(new TimerQueue(this)),
                                 wakeupFd_(createEventfd()),
                                 wakeupChannel_(new Channel(this, wakeupFd_))
        {
            LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
            if(t_loopInThisThread){
                LOG_FATAL << "Another EventLoop " << t_loopInThisThread << " exists in this thread " << threadId_;
            }else{
                t_loopInThisThread = this;
            }
            wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
            wakeupChannel_->enableReading();
        }
        EventLoop::~EventLoop(){
            LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
                      << " destructs in thread " << CurrentThread::tid();
            ownedObjects_.clear();
            wakeupChannel_->disableAll();
            wakeupChannel_->remove();
            ::close(wakeupFd_);
            t_loopInThisThread = nullptr;
        }
        void EventLoop::loop(){
            assert(!looping_);
            assertInLoopThread();
            looping_ = true;
            quit_.set(0);
            while(quit_.get()==0){
                activeChannels_.clear();
                pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
                eventHandling_ = true;
                for(Channel*channel:activeChannels_){
                    channel->handleEvent(pollReturnTime_);
                }
                eventHandling_ = false;
                doPendingFunctors();
            }
            looping_ = false;
        }
        void EventLoop::quit(){
            quit_.set(1);
            if(!isInLoopThread()){
                wakeup();
            }
        }
        void EventLoop::runInLoop(Functor cb){
            if(isInLoopThread()){
                cb();
            }else{
                queueInLoop(std::move(cb));
            }
        }
        void EventLoop::queueInLoop(Functor cb){
            {
                MutexLockGuard lock(mutex_);
                pendingFunctors_.push_back(std::move(cb));
            }
            if(!isInLoopThread()||callingPendingFunctors_){
                wakeup();
            }
        }
        void EventLoop::runInLoopSync(Functor cb){
            if(isInLoopThread()){
                cb();
                return;
            }
            MutexLock mtx;
            Condition cv(mtx);
            bool done = false;
            queueInLoop([&]
                        { cb();
                        MutexLockGuard lock(mtx);
                    done=true;
                cv.notify(); });
            MutexLockGuard lock(mtx);
            while(!done){
                cv.wait();
            }
        }
        size_t EventLoop::queueSize()const{
            MutexLockGuard locj(mutex_);
            return pendingFunctors_.size();
        }
        void EventLoop::wakeup(){
            uint64_t one = 1;
            ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
            if(n!=sizeof(one)){
                LOG_ERROR << "EventLoop::wakeup() writes " << n
                          << " bytes instead of 8";
            }
        }
        void EventLoop::handleRead(){
            uint64_t one = 1;
            ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
            if (n != sizeof(one))
            {
                LOG_ERROR << "EventLoop::handleRead() reads " << n
                          << " bytes instead of 8";
            }
        }
        void EventLoop::doPendingFunctors(){
            std::vector<Functor> functors;
            callingPendingFunctors_ = true;
            {
                MutexLockGuard lock(mutex_);
                functors.swap(pendingFunctors_);
            }
            for(const Functor&functor:functors){
                functor();
            }
            callingPendingFunctors_ = false;
        }
        void EventLoop::updateChannel(Channel *channel){
            assert(channel->ownerLoop() == this);
            assertInLoopThread();
            poller_->updateChannel(channel);
        }
        void EventLoop::removeChannel(Channel *channel)
        {
            assert(channel->ownerLoop() == this);
            assertInLoopThread();
            poller_->removeChannel(channel);
        }

        bool EventLoop::hasChannel(Channel *channel)
        {
            assert(channel->ownerLoop() == this);
            assertInLoopThread();
            return poller_->hasChannel(channel);
        }
        void EventLoop::abortNotInLoopThread()
        {
            LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
                      << " was created in threadId_ = " << threadId_
                      << ", current thread id = " << CurrentThread::tid();
        }
        TimerId EventLoop::runAt(Timestamp time,TimerCallback cb){
            return timerQueue_->addTimer(std::move(cb), time, 0.0);
        }
        TimerId EventLoop::runAfter(double delay, TimerCallback cb)
        {
            Timestamp time(addTime(Timestamp::now(), delay));
            return runAt(time, std::move(cb));
        }

        TimerId EventLoop::runEvery(double interval, TimerCallback cb)
        {
            Timestamp time(addTime(Timestamp::now(), interval));
            return timerQueue_->addTimer(std::move(cb), time, interval);
        }

        void EventLoop::cancel(TimerId timerId)
        {
            return timerQueue_->cancel(timerId);
        }
    }
}
