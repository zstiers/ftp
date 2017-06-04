#ifndef FTP_d6f2888091aa482e92de81684ddb3ab7
#define FTP_d6f2888091aa482e92de81684ddb3ab7

#include "ext\concurrentqueue\concurrentqueue.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ftp
{
    // Not required to be used, but deriving from this is allowed.
    struct ThreadInitializer
    {
        virtual void OnStart (std::size_t /* threadIndex */) { }
        virtual void OnStop (std::size_t /* threadIndex */)  { }
    };

    enum class RemoveBehavior { DETATCH, JOIN, NONE, };
    enum class WorkBehavior   { STOP, COMPLETE, CONTINUE, };

    class ThreadPool
    {
        typedef std::function<void()> FuncType;
        std::condition_variable                                 m_cv;
        ThreadInitializer *                                     m_initializer = nullptr;
        std::mutex                                              m_mutex;
        moodycamel::ConcurrentQueue<FuncType>                   m_queue;
        std::atomic<std::size_t>                                m_taskCount = 0;
        std::vector<std::thread>                                m_threads;
        std::atomic<std::size_t>                                m_waiting = 0;
        std::vector<std::shared_ptr<std::atomic<WorkBehavior>>> m_workBehavior;

    public:
        ThreadPool (std::size_t startingCount = 0, ThreadInitializer * threadInitializer = nullptr) :
            m_initializer(threadInitializer)
        {
            Resize(startingCount, WorkBehavior::CONTINUE, RemoveBehavior::JOIN, nullptr);
        }

        ~ThreadPool ()
        {
            Stop();
        }

    public:
        std::size_t GetThreadCount () const { return m_threads.size(); }

    public:
        bool Pop (FuncType & out)
        {
            // See if we can take anything. If the value is zero we have nothing we can pop,
            // but if the value is non-zero and we successfully subtract from it then we can
            // pop.
            for (std::size_t oldValue = m_taskCount.load(); oldValue;)
            {
                if (m_taskCount.compare_exchange_weak(oldValue, oldValue - 1, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    // Because of the way the concurrent queue talks between threads
                    // the queued data might not have been immediate. In this case we
                    // will wait. We know it has something for us.
                    for (;;)
                    {
                        if (m_queue.try_dequeue(out))
                            return true;
                    }
                }
            }
            return false;
        }

        FuncType Pop ()
        {
            FuncType func;
            Pop(func);
            return func;
        }

        void Push (FuncType && func)
        {
            m_queue.enqueue(func);
            m_taskCount.fetch_add(1, std::memory_order_relaxed);

            // This may look a little weird, but this works. This elimates a race condition
            // where we might be in the process of trying to put a thread to sleep at the
            // same time as waking up another by forcing this thread to wait until the thread
            // is actually asleep before sending the notification.
            if (m_waiting.load(std::memory_order_acquire) == 1)
                std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_one();
        }

    public:
        void Stop ()
        {
            Resize(0, WorkBehavior::COMPLETE, RemoveBehavior::JOIN, nullptr);
        }

    public:
        void Resize (std::size_t newCount, WorkBehavior workBehavior, RemoveBehavior removeBehavior, std::vector<std::thread> * removed)
        {
            if (newCount < 0)
                newCount = 0;

            std::size_t oldCount = GetThreadCount();
            if (newCount > oldCount)
            {
                // Need to create the new threads.
                m_workBehavior.resize(newCount);
                m_threads.resize(newCount);
                for (std::size_t i = oldCount; i < newCount; ++i)
                {
                    m_workBehavior[i] = std::make_shared<std::atomic<WorkBehavior>>(WorkBehavior::CONTINUE);
                    StartThread(i);
                }
            }
            else if (oldCount > newCount)
            {
                // Tell the old threads they are done.
                for (std::size_t i = newCount; i < oldCount; ++i)
                    *m_workBehavior[i] = workBehavior;
                m_workBehavior.resize(newCount);

                // Need to very briefly lock the mutex to make sure no threads are
                // in race conditions from not seeing the running state update yet.
                m_mutex.lock();
                m_mutex.unlock();

                // Notification is to allow the destroyed threads to close themselves out.
                m_cv.notify_all();
             
                 // Stop the actual threads.
                for (std::size_t i = newCount; i < oldCount; ++i)
                {
                    std::thread & thread = m_threads[i];
                    if (removeBehavior == RemoveBehavior::DETATCH)
                        thread.detach();
                    else if (removeBehavior == RemoveBehavior::JOIN)
                        thread.join();
                    if (removed)
                        removed->push_back(std::move(thread));
                }
                m_threads.resize(newCount);
            }
        }

    private:
        void ThreadLoop (std::atomic<WorkBehavior> & workBehavior)
        {
            // Variables we are going to reuse.
            FuncType func;
            bool hasWork = Pop(func);
            const auto cvFunc = [this, &func, &hasWork, &workBehavior]() {
                hasWork = Pop(func);
                return hasWork || workBehavior.load(std::memory_order_relaxed) != WorkBehavior::CONTINUE;
            };

            for (;;) {
                while (hasWork) {
                    func();

                    if (workBehavior.load(std::memory_order_relaxed) != WorkBehavior::STOP)
                        hasWork = Pop(func);
                    else
                        return;
                }

                // the queue is empty here, wait for the next command
                std::unique_lock<std::mutex> lock(m_mutex);
                m_waiting.fetch_add(1, std::memory_order_release);
                m_cv.wait(lock, cvFunc);
                m_waiting.fetch_sub(1, std::memory_order_relaxed);

                if (!hasWork)
                    return;
            }
        }

        void StartThread (std::size_t threadIndex)
        {
            std::shared_ptr<std::atomic<WorkBehavior>> workBehavior = m_workBehavior[threadIndex]; // Take a copy
            auto f = [this, threadIndex, workBehavior]() {
                if (m_initializer) m_initializer->OnStart(threadIndex);
                ThreadLoop(*workBehavior);
                if (m_initializer) m_initializer->OnStop(threadIndex);
            };
            m_threads[threadIndex] = std::thread(f);
        }
    };
} // namespace ftp

#endif // FTP_d6f2888091aa482e92de81684ddb3ab7