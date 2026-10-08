#pragma once
// Fixed number of worker threads pulling client sockets off a BoundedQueue.
// Max work in the system = num_threads (being served) + queue_capacity (waiting).

#include <functional>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"

class ThreadPool {
public:
    using Handler = std::function<void(int client_fd)>;

    ThreadPool(std::size_t num_threads, std::size_t queue_capacity, Handler handler)
        : queue_(queue_capacity), handler_(std::move(handler)) {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // false => pool is saturated; caller should reply 503.
    bool try_submit(int client_fd) { return queue_.try_push(client_fd); }

    // Close the queue, let workers finish in-flight + queued work, then join.
    // Safe to call more than once.
    void shutdown() {
        queue_.close();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void worker_loop() {
        while (auto fd = queue_.pop()) {  // nullopt => closed and drained
            handler_(*fd);
        }
    }

    BoundedQueue<int> queue_;
    Handler handler_;
    std::vector<std::thread> workers_;
};
