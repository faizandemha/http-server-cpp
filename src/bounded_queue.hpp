#pragma once
// A fixed-capacity FIFO shared by one producer (the accept loop) and many
// consumers (worker threads). Classic producer-consumer with a mutex + condvar.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // NON-blocking push. Returns false if the queue is full (or closed).
    // The accept loop must never stall, so instead of waiting for space
    // it gets an immediate "no" and answers the client with 503.
    bool try_push(T item) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_ || items_.size() >= capacity_) return false;
            items_.push_back(std::move(item));
        }
        // Notify after releasing the lock so the woken worker doesn't
        // immediately block on a mutex we still hold.
        not_empty_.notify_one();
        return true;
    }

    // Blocking pop. Sleeps until there is work or the queue is closed.
    // Returns nullopt only when closed AND empty -> the worker should exit.
    // Because of that, already-queued connections are still drained on shutdown.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mu_);
        // The predicate guards against spurious wakeups.
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Stop accepting new items and wake every sleeping worker.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    const std::size_t capacity_;
    bool closed_ = false;
};
