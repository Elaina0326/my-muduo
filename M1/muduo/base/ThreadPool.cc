#include "ThreadPool.h"

#include "Logging.h"
#include <cassert>
#include <cstdio>
#include <exception>

namespace muduo
{
    ThreadPool::ThreadPool(const std::string &nameArg) : mutex_(), notEmpty_(mutex_), notFull_(mutex_), name_(nameArg) {}

    ThreadPool::~ThreadPool()
    {
        if (running_)
        {
            stop();
        }
    }

    void ThreadPool::start(int numThreads)
    {
        if (!threads_.empty())
        {
            LOG_FATAL << "ThreadPool already started";
        }
        running_ = true;
        threads_.reserve(static_cast<size_t>(numThreads));

        for (int i = 0; i < numThreads; ++i)
        {
            char id[32];
            std::snprintf(id, sizeof(id), "%d", i);
            threads_.emplace_back(new muduo::Thread(std::bind(&ThreadPool::runInThread, this), name_ + id));
            threads_[static_cast<size_t>(i)]->start();
        }
        if (numThreads == 0 && threadInitCallback_)
        {
            threadInitCallback_();
        }
    }
    void ThreadPool::stop()
    {
        {
            MutexLockGuard lock(mutex_);
            running_ = false;
            notEmpty_.notifyAll();
            notFull_.notifyAll();
        }
        for (auto &thr : threads_)
        {
            thr->join();
        }
    }

    size_t ThreadPool::queueSize() const
    {
        MutexLockGuard lock(mutex_);
        return queue_.size();
    }

    void ThreadPool::run(Task task)
    {
        if (threads_.empty())
        {
            task();
            return;
        }
        MutexLockGuard lock(mutex_);
        while (running_ && maxQueueSize_ > 0 && queue_.size() >= maxQueueSize_)
        {
            notFull_.wait();
        }
        if (!running_)
        {
            return;
        }
        queue_.push_back(std::move(task));
        notEmpty_.notify();
    }
    bool ThreadPool::take(Task *task)
    {
        MutexLockGuard lock(mutex_);

        while (queue_.empty() && running_)
        {
            notEmpty_.wait();
        }
        if (queue_.empty())
        {
            return false;
        }

        *task = std::move(queue_.front());
        queue_.pop_front();
        if (maxQueueSize_ > 0)
        {
            notFull_.notify();
        }
        return true;
    }

    void ThreadPool::runInThread()
    {
        try
        {
            if (threadInitCallback_)
            {
                threadInitCallback_();
            }
        }catch (const std::exception &e){
            LOG_ERROR << "ThreadPool[" << name_ << "]init callback threw: " << e.what();

        }catch(...){
            LOG_ERROR << "ThreadPool[" << name_ << "]init callback threw unknown exception";

        }
        Task task;
        while (take(&task))
        {
            try
            {
                task();
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "ThreadPool[" << name_ << "]task threw: " << e.what();
            }
            catch (...)
            {
                LOG_ERROR << "ThreadPool[" << name_ << "]task threw unknown exception";

            }
            task = Task(); // 尽早释放
        }
    }
}
