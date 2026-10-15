#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
public:
    ThreadPool(std::size_t workers, std::size_t capacity) : capacity_(capacity) {
        if (!workers || !capacity) throw std::invalid_argument("线程数和队列容量必须大于零");
        try {
            for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { run(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool() { shutdown(); }
    bool tryEnqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || tasks_.size() == capacity_) return false;
            tasks_.push(std::move(task));
        }
        ready_.notify_one();
        return true;
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front()); tasks_.pop();
            }
            task();
        }
    }
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};
