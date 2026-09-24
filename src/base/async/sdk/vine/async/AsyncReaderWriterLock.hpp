#pragma once

#include "async_global.hpp"

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <utility>

VN_ASYNC_NS_BEGIN

class AsyncReaderWriterLock;

/**
 * @brief RAII guard releasing a held read lock on destruction.
 */
class AsyncReaderGuard
{
  public:
    AsyncReaderGuard() noexcept = default;

    explicit AsyncReaderGuard(AsyncReaderWriterLock* lock) noexcept : lock_(lock) {}

    AsyncReaderGuard(AsyncReaderGuard&& other) noexcept
      : lock_(std::exchange(other.lock_, nullptr))
    {}

    AsyncReaderGuard& operator=(AsyncReaderGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (lock_)
            {
                release();
            }
            lock_ = std::exchange(other.lock_, nullptr);
        }
        return *this;
    }

    ~AsyncReaderGuard()
    {
        if (lock_)
        {
            release();
        }
    }

    AsyncReaderGuard(const AsyncReaderGuard&) = delete;
    AsyncReaderGuard& operator=(const AsyncReaderGuard&) = delete;

  private:
    void release() noexcept;

    AsyncReaderWriterLock* lock_{ nullptr };
};

/**
 * @brief RAII guard releasing a held write lock on destruction.
 */
class AsyncWriterGuard
{
  public:
    AsyncWriterGuard() noexcept = default;

    explicit AsyncWriterGuard(AsyncReaderWriterLock* lock) noexcept : lock_(lock) {}

    AsyncWriterGuard(AsyncWriterGuard&& other) noexcept
      : lock_(std::exchange(other.lock_, nullptr))
    {}

    AsyncWriterGuard& operator=(AsyncWriterGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (lock_)
            {
                release();
            }
            lock_ = std::exchange(other.lock_, nullptr);
        }
        return *this;
    }

    ~AsyncWriterGuard()
    {
        if (lock_)
        {
            release();
        }
    }

    AsyncWriterGuard(const AsyncWriterGuard&) = delete;
    AsyncWriterGuard& operator=(const AsyncWriterGuard&) = delete;

  private:
    void release() noexcept;

    AsyncReaderWriterLock* lock_{ nullptr };
};

/**
 * @brief Async reader-writer lock with writer preference.
 *
 * Multiple readers or one writer may hold the lock. Waiting writers are served
 * before new readers (writer preference, preventing writer starvation); when a
 * lock is released, queued writers wake first, otherwise all queued readers
 * wake together. This is writer-preference, not strict FIFO: once a writer is
 * queued, later readers do not acquire until the writer queue empties.
 *
 * Lifetime: the lock must outlive every coroutine waiting on it and every
 * guard obtained from it. Waiters unregister on destruction, so an abandoned
 * wait is safe.
 *
 * Usage: auto guard = co_await lock.readerLock(); (or writerLock()); the guard
 * releases on destruction.
 *
 * Internal invariants (guarded by mutex_):
 *   writer_                 => readers_ == 0
 *   readers_ > 0            => !writer_
 *   writers_head_           => new readers cannot acquire
 *   readers_head_ == nullptr => readers_tail_ == nullptr
 *   writers_head_ == nullptr => writers_tail_ == nullptr
 * Never resume a waiter while holding mutex_: the resumed coroutine may
 * immediately acquire or release this lock. Granted readers are resumed one
 * at a time (never holding a waiter pointer across a resume), so a resumed
 * reader whose completion destroys a sibling waiter is safe: the destroyed
 * sibling unregisters itself from the shared list.
 */
class AsyncReaderWriterLock
{
  public:
    AsyncReaderWriterLock() noexcept = default;

    AsyncReaderWriterLock(const AsyncReaderWriterLock&) = delete;
    AsyncReaderWriterLock& operator=(const AsyncReaderWriterLock&) = delete;

    /**
     * @brief Awaiter that acquires a read lock and yields a guard.
     */
    class ReaderAwaiter
    {
      public:
        explicit ReaderAwaiter(AsyncReaderWriterLock& lock) noexcept : lock_(lock) {}

        ReaderAwaiter(const ReaderAwaiter&) = delete;
        ReaderAwaiter& operator=(const ReaderAwaiter&) = delete;

        ~ReaderAwaiter()
        {
            if (waiter_)
            {
                std::lock_guard<std::mutex> lock(lock_.mutex_);
                lock_.dequeueReader(*this);
            }
        }

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return lock_.tryAcquireReader();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept
        {
            // await_ready() is only an optimistic fast path; the state must be
            // re-checked under mutex_ here, or a release between the two checks
            // could be lost and this coroutine would never wake.
            waiter_ = h;
            std::lock_guard<std::mutex> lock(lock_.mutex_);
            if (!lock_.writer_ && !lock_.writers_head_)
            {
                ++lock_.readers_;
                waiter_ = {};
                return false; // Acquired inline.
            }
            lock_.enqueueReader(*this);
            return true;
        }

        [[nodiscard]]
        AsyncReaderGuard await_resume() noexcept
        {
            return AsyncReaderGuard{ &lock_ };
        }

      private:
        friend class AsyncReaderWriterLock;

        AsyncReaderWriterLock&  lock_;
        std::coroutine_handle<> waiter_{};
        /// true after a release granted this reader's slot but before it is
        /// resumed; distinguishes granted readers from later-queued ones.
        bool                    granted_{ false };
        ReaderAwaiter*          next_{ nullptr };
        ReaderAwaiter*          prev_{ nullptr };
    };

    /**
     * @brief Awaiter that acquires a write lock and yields a guard.
     */
    class WriterAwaiter
    {
      public:
        explicit WriterAwaiter(AsyncReaderWriterLock& lock) noexcept : lock_(lock) {}

        WriterAwaiter(const WriterAwaiter&) = delete;
        WriterAwaiter& operator=(const WriterAwaiter&) = delete;

        ~WriterAwaiter()
        {
            if (waiter_)
            {
                std::lock_guard<std::mutex> lock(lock_.mutex_);
                lock_.dequeueWriter(*this);
            }
        }

        [[nodiscard]]
        bool await_ready() const noexcept
        {
            return lock_.tryAcquireWriter();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept
        {
            // await_ready() is only an optimistic fast path; the state must be
            // re-checked under mutex_ here, or a release between the two checks
            // could be lost and this coroutine would never wake.
            waiter_ = h;
            std::lock_guard<std::mutex> lock(lock_.mutex_);
            if (lock_.readers_ == 0 && !lock_.writer_)
            {
                lock_.writer_ = true;
                waiter_ = {};
                return false; // Acquired inline.
            }
            lock_.enqueueWriter(*this);
            return true;
        }

        [[nodiscard]]
        AsyncWriterGuard await_resume() noexcept
        {
            return AsyncWriterGuard{ &lock_ };
        }

      private:
        friend class AsyncReaderWriterLock;

        AsyncReaderWriterLock&  lock_;
        std::coroutine_handle<> waiter_{};
        WriterAwaiter*          next_{ nullptr };
        WriterAwaiter*          prev_{ nullptr };
    };

    /**
     * @brief Returns an awaiter that acquires a read lock.
     *
     * @return An awaiter; co_await it to hold a read lock until the guard dies.
     */
    [[nodiscard]]
    ReaderAwaiter readerLock() noexcept
    {
        return ReaderAwaiter{ *this };
    }

    /**
     * @brief Returns an awaiter that acquires a write lock.
     *
     * @return An awaiter; co_await it to hold a write lock until the guard dies.
     */
    [[nodiscard]]
    WriterAwaiter writerLock() noexcept
    {
        return WriterAwaiter{ *this };
    }

    /**
     * @brief Releases a read lock, waking queued writers or readers.
     */
    void unlockRead() noexcept;

    /**
     * @brief Releases a write lock, waking queued writers or readers.
     */
    void unlockWrite() noexcept;

  private:
    friend class AsyncReaderGuard;
    friend class AsyncWriterGuard;
    friend class ReaderAwaiter;
    friend class WriterAwaiter;

    bool tryAcquireReader() noexcept;
    bool tryAcquireWriter() noexcept;
    void enqueueReader(ReaderAwaiter& a) noexcept;
    void dequeueReader(ReaderAwaiter& a) noexcept;
    void enqueueWriter(WriterAwaiter& a) noexcept;
    void dequeueWriter(WriterAwaiter& a) noexcept;

    /// Waiters to resume after a release, collected under mutex_.
    struct WakeBatch
    {
        WriterAwaiter* writer{ nullptr };
        /// true when queued readers were granted and must be resumed.
        bool           wake_readers{ false };
    };

    /// Detaches one queued writer or all queued readers; mutex_ must be held.
    WakeBatch takeWaitersLocked() noexcept;

    /// Resumes a WakeBatch; locks mutex_ only to pop one granted reader at a time.
    void resume(WakeBatch batch) noexcept;

    std::mutex     mutex_{};
    std::size_t    readers_{ 0 };
    std::size_t    waiting_readers_{ 0 };
    bool           writer_{ false };
    ReaderAwaiter* readers_head_{ nullptr };
    ReaderAwaiter* readers_tail_{ nullptr };
    WriterAwaiter* writers_head_{ nullptr };
    WriterAwaiter* writers_tail_{ nullptr };
};

inline void AsyncReaderGuard::release() noexcept
{
    lock_->unlockRead();
    lock_ = nullptr;
}

inline void AsyncWriterGuard::release() noexcept
{
    lock_->unlockWrite();
    lock_ = nullptr;
}

inline bool AsyncReaderWriterLock::tryAcquireReader() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writer_ && !writers_head_)
    {
        ++readers_;
        return true;
    }
    return false;
}

inline bool AsyncReaderWriterLock::tryAcquireWriter() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (readers_ == 0 && !writer_)
    {
        writer_ = true;
        return true;
    }
    return false;
}

inline void AsyncReaderWriterLock::unlockRead() noexcept
{
    WakeBatch batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(readers_ > 0);
        --readers_;
        if (readers_ == 0)
        {
            batch = takeWaitersLocked();
        }
    }
    resume(batch);
}

inline void AsyncReaderWriterLock::unlockWrite() noexcept
{
    WakeBatch batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(writer_);
        writer_ = false;
        batch = takeWaitersLocked();
    }
    resume(batch);
}

inline AsyncReaderWriterLock::WakeBatch AsyncReaderWriterLock::takeWaitersLocked() noexcept
{
    WakeBatch batch;
    if (writers_head_)
    {
        batch.writer = writers_head_;
        writers_head_ = batch.writer->next_;
        if (!writers_head_)
        {
            writers_tail_ = nullptr;
        }
        else
        {
            // Detach the old head from its successor so a later dequeue of the
            // successor cannot write through the old head's freed frame.
            writers_head_->prev_ = nullptr;
        }
        batch.writer->next_ = nullptr;
        batch.writer->prev_ = nullptr;
        writer_ = true; // The woken writer now holds the lock.
    }
    else
    {
        // Grant every queued reader at once and mark each as granted. They
        // stay in the shared list so the resume loop can pop one at a time
        // and so an abandoned (destroyed) granted reader unregisters itself.
        batch.wake_readers = true;
        for (auto* a = readers_head_; a; a = a->next_)
        {
            a->granted_ = true;
        }
        readers_ += waiting_readers_; // Grant the woken readers.
        waiting_readers_ = 0;
    }
    return batch;
}

inline void AsyncReaderWriterLock::resume(WakeBatch batch) noexcept
{
    if (batch.writer)
    {
        auto h = batch.writer->waiter_;
        batch.writer->waiter_ = {};
        h.resume();
        return;
    }
    if (!batch.wake_readers)
    {
        return;
    }
    // All readers were granted at once (readers_ already includes them). Pop
    // one granted reader per lock and resume it, so a resumed reader whose
    // completion destroys a still-suspended sibling (e.g. a whenAny winner)
    // never leaves a stale pointer: the destroyed sibling unregisters itself
    // and is simply not found on the next iteration. Readers that queued after
    // the grant sit behind the granted ones and are never popped.
    for (;;)
    {
        ReaderAwaiter* a = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (readers_head_ && readers_head_->granted_)
            {
                a = readers_head_;
                readers_head_ = a->next_;
                if (!readers_head_)
                {
                    readers_tail_ = nullptr;
                }
                else
                {
                    readers_head_->prev_ = nullptr;
                }
                a->next_ = nullptr;
                a->prev_ = nullptr;
                a->granted_ = false;
            }
        }
        if (!a)
        {
            break;
        }
        auto h = a->waiter_;
        a->waiter_ = {};
        h.resume(); // Resume outside the lock.
    }
}

inline void AsyncReaderWriterLock::enqueueReader(ReaderAwaiter& a) noexcept
{
    ++waiting_readers_;
    a.granted_ = false;
    a.prev_ = readers_tail_;
    a.next_ = nullptr;
    if (readers_tail_)
    {
        readers_tail_->next_ = &a;
    }
    else
    {
        readers_head_ = &a;
    }
    readers_tail_ = &a;
}

inline void AsyncReaderWriterLock::dequeueReader(ReaderAwaiter& a) noexcept
{
    if (a.prev_)
    {
        a.prev_->next_ = a.next_;
    }
    else if (readers_head_ == &a)
    {
        readers_head_ = a.next_;
    }
    else
    {
        return;
    }

    if (a.next_)
    {
        a.next_->prev_ = a.prev_;
    }
    else if (readers_tail_ == &a)
    {
        readers_tail_ = a.prev_;
    }

    if (a.granted_)
    {
        --readers_; // Destroyed before resuming; its granted slot is returned.
    }
    else
    {
        --waiting_readers_;
    }
    a.prev_ = nullptr;
    a.next_ = nullptr;
}

inline void AsyncReaderWriterLock::enqueueWriter(WriterAwaiter& a) noexcept
{
    a.prev_ = writers_tail_;
    a.next_ = nullptr;
    if (writers_tail_)
    {
        writers_tail_->next_ = &a;
    }
    else
    {
        writers_head_ = &a;
    }
    writers_tail_ = &a;
}

inline void AsyncReaderWriterLock::dequeueWriter(WriterAwaiter& a) noexcept
{
    if (a.prev_)
    {
        a.prev_->next_ = a.next_;
    }
    else if (writers_head_ == &a)
    {
        writers_head_ = a.next_;
    }
    else
    {
        return;
    }

    if (a.next_)
    {
        a.next_->prev_ = a.prev_;
    }
    else if (writers_tail_ == &a)
    {
        writers_tail_ = a.prev_;
    }

    a.prev_ = nullptr;
    a.next_ = nullptr;
}

VN_ASYNC_NS_END
