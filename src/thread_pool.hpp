// Fixed-size worker pool with a bounded task queue.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    ThreadPool(std::size_t workers, std::size_t max_queue) : max_queue_(max_queue) {
        if (workers == 0) workers = 1;
        for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { loop(); });
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Returns false when the queue is full or the pool is stopping, so the
    // caller can shed load (e.g. answer 503) instead of growing without bound.
    bool try_submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_ || tasks_.size() >= max_queue_) return false;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    // Stops accepting work, lets queued tasks finish, then joins the workers.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) return;  // stopping and drained
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "worker task threw: " << e.what() << '\n';
            } catch (...) {
                std::cerr << "worker task threw an unknown exception\n";
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    std::size_t max_queue_;
    bool stopping_ = false;
};
