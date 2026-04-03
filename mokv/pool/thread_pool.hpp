#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <utility>
#include <vector>
#include <condition_variable>
#include <string>
#include <thread>
#include <future>
#include <mutex>

namespace cpputil {

namespace pool {

/*
optimized bytedance cpputil/pool
*/ 
class ThreadPool {

public:
    ThreadPool(size_t thread_num, const std::string& name = "default_pool", size_t max_queue_size = 1000)
    : worker_num_(thread_num), max_queue_size_(max_queue_size) {
        workers_.reserve(thread_num);
        for (size_t i = 0; i < thread_num; i++) {
            std::string thread_name = name + "_thread_" + std::to_string(i);
            workers_.emplace_back([this, thread_name] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex_);
                        free_worker_num_++;
                        this->condition_.wait(lock, [this] { return this->stop_ || !this->tasks_.empty(); });
                        free_worker_num_--;
                        if (this->stop_ && this->tasks_.empty()) {
                            return;
                        }
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                        queue_size_--;
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    template <typename F, typename... Args>
    auto Enqueue(F &&f, Args&& ...args) -> std::future<std::invoke_result_t<F, Args...> > {
        using return_type = typename std::invoke_result_t<F, Args...>;
        
        auto task = new std::packaged_task<return_type()>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            
            tasks_.emplace([task]() {
                (*task)();
                delete task;
            });

            queue_size_++;
        }

        condition_.notify_one();
        return future;
    }

    template <typename ReturnType>
    void MultiEnqueue(std::vector<std::function<ReturnType()> >& functions, std::vector<std::future<ReturnType> >& futures) {
        std::vector<std::packaged_task<ReturnType()>* > packaged_tasks;
        packaged_tasks.reserve(functions.size());
        futures.reserve(functions.size());
        for (size_t i = 0; i < functions.size(); i++) {
            packaged_tasks.emplace_back(new std::packaged_task<ReturnType()>(functions[i]));
            futures.emplace_back(packaged_tasks[i]->get_future());
        }
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            for (auto task : packaged_tasks) {
                tasks_.emplace([task]() {
                    (*task)();
                    delete task;
                });
            }
            queue_size_ += functions.size();
        }
        for (size_t i = 0; i < functions.size(); i++) {
            condition_.notify_one();
        }
    }

    template <typename ReturnType>
    inline void ConcurrentRun(std::vector<std::function<ReturnType()> >& functions) {
        std::vector<std::future<ReturnType> > futures;
        futures.reserve(functions.size());
        MultiEnqueue(functions, futures);
        for (auto&& result : futures) {
            std::move(result).get();
        }
    }

    template <typename ReturnType, typename ValueType>
    inline void ConcurrentRun(std::vector<std::function<ReturnType()> >& functions, std::vector<ValueType>& results) {
        std::vector<std::future<ReturnType> > futures;
        futures.reserve(functions.size());
        MultiEnqueue(functions, futures);
        for (auto&& result : futures) {
            results.emplace_back(std::move(result).get());
        }
    }

    inline bool IsBusy() {
        return queue_size_ > 5;
    }


private:
    // can not resize or emplace
    std::vector<std::thread> workers_;
    
    std::queue<std::function<void()> > tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_{false};
    size_t worker_num_;
    size_t free_worker_num_{0};
    size_t max_queue_size_;
    size_t queue_size_{0};
};

} // pool

} // cpputil