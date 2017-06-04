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

    public: // Ctor & Dtor
        ThreadPool (std::size_t startingCount = 0, ThreadInitializer * threadInitializer = nullptr) :
            m_initializer(threadInitializer)
        {
            Resize(startingCount, WorkBehavior::CONTINUE, RemoveBehavior::JOIN, nullptr);
        }

        ~ThreadPool ()
        {
            Stop();
        }
        
    public: // Commands
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

        bool Push (const FuncType & func)
        {
            const bool success = m_queue.enqueue(func);
            OnEnqueue(1);
            return success;
        }

        bool Push (FuncType && func)
        {
            const bool success = m_queue.enqueue(std::forward<FuncType>(func));
            OnEnqueue(1);
            return success;
        }

        // Pushes several items.
	    // Note: Use std::make_move_iterator if the elements should be moved instead of copied.
	    template<typename It>
	    bool Push (It itemFirst, size_t count)
        {
            const bool success = m_queue.enqueue_bulk(itemFirst, count);
            OnEnqueue(count);
            return success;
        }

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
                LockTemp();

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

        void Stop ()
        {
            Resize(0, WorkBehavior::COMPLETE, RemoveBehavior::JOIN, nullptr);
        }

    public: // Queries
        std::size_t GetThreadCount () const { return m_threads.size(); }

    private: // Internal helpers
        void LockTemp ()
        {
            // There are a number of cases we don't really need a lock, but
            // do need to temporarily make sure nothing else has it. This is
            // used in cases that we have invalidated variables checked inside
            // locks and need to wait before firing notifications because of this.
            m_mutex.lock();
            m_mutex.unlock();
        }

        void OnEnqueue (std::size_t count)
        {
            m_taskCount.fetch_add(count, std::memory_order_relaxed);

            // Before notification we have to check if the number waiting is
            // equal to the count, clamped to the maximum number of threads,
            // and temporarily lock our mutex if so. This is because it is possible
            // for one thread to not wake up if we don't do this as it could
            // be currently running checks we just invalidated.
            auto numWaiting = m_waiting.load(std::memory_order_acquire);
            if (count == 1)
            {
                if (numWaiting == 1)
                    LockTemp();
                m_cv.notify_one();
            }
            else
            {
                m_cv.notify_all();

                // Similar test to the branch above. This though clamps to the
                // thread count. We notify all before this test followed by a
                // single notify because letting the other threads start on work
                // before we do this is helpful.
                auto threadCount = GetThreadCount();
                if (numWaiting == (count <= threadCount ? count : threadCount))
                {
                    LockTemp();
                    m_cv.notify_one();
                }
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
    };
} // namespace ftp

#endif // FTP_d6f2888091aa482e92de81684ddb3ab7