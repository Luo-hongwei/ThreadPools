#ifndef THREADPOOL_H  // 防止重复包含头文件
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <memory>
#include <string>

enum class RejectionPolicy {
    ABORT,
    CALLER_RUNS,
    DISCARD,
    DISCARD_OLDEST 
};

class ThreadPool {
public:
    virtual void enqueue(const std::function<void()>& task) = 0; // 添加任务
    virtual void shutdown() = 0; // 平缓关闭
    virtual std::vector<std::thread::id> shutdownNow() = 0; // 暴力关闭，返回正在运行线程的ID
    virtual ~ThreadPool() = default; // 虚析构函数
};

class FixedThreadPool : public ThreadPool {
public:
    FixedThreadPool(size_t numThreads, size_t queueSize, RejectionPolicy policy = RejectionPolicy::ABORT, 
                    bool allowCoreThreadTimeout = false, std::chrono::milliseconds coreThreadTimeout = std::chrono::milliseconds(10000))
        : stop(false), maxQueueSize(queueSize), rejectionPolicy(policy),
          allowCoreThreadTimeout(allowCoreThreadTimeout), coreThreadTimeout(coreThreadTimeout) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&FixedThreadPool::worker, this); // 启动工作线程
        }
    }

    void enqueue(const std::function<void()>& task) override {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (tasks.size() >= maxQueueSize) {
                handleRejection(task); // 处理拒绝逻辑
                return;
            }
            tasks.push(task); // 添加任务到队列
        }
        condition.notify_one(); // 通知至少有一个线程可以工作
    }

    void shutdown() override {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true; // 设置停止标志
        }
        condition.notify_all(); // 通知所有线程
        for (std::thread &worker : workers) {
            worker.join(); // 等待所有线程结束
        }
    }

    std::vector<std::thread::id> shutdownNow() override {
        std::vector<std::thread::id> runningThreads;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true; // 设置停止标志
            for (const auto& worker : workers) {
                runningThreads.push_back(worker.get_id()); // 获取正在运行的线程ID
            }
        }
        condition.notify_all(); // 通知所有线程立即退出
        return runningThreads; // 返回正在运行的线程ID列表
    }

    ~FixedThreadPool() {
        shutdown(); // 安全关闭
    }

private:
    void worker() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(this->queueMutex);
                this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                if (this->stop && this->tasks.empty()) {
                    return; // 如果停止且队列为空，退出
                }
                task = std::move(this->tasks.front()); // 获取任务
                this->tasks.pop(); // 从队列中移除任务
                lastActiveTime = std::chrono::steady_clock::now(); // 更新最后活跃时间
            }
            try {
                task(); // 执行任务
            } catch (const std::exception& e) {
                std::cerr << "Task execution failed: " << e.what() << std::endl; // 捕获并处理任务异常
            }

            if (allowCoreThreadTimeout) {
                checkForCoreThreadTimeout();
            }
        }
    }

    void checkForCoreThreadTimeout() {
        std::unique_lock<std::mutex> lock(queueMutex);
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastActiveTime) > coreThreadTimeout) {
            std::cout << "Core thread timeout, consider termination." << std::endl;
        }
    }

    void handleRejection(const std::function<void()>& task) {
        switch (rejectionPolicy) {
            case RejectionPolicy::ABORT:
                throw std::runtime_error("Task queue is full, task rejected"); // 抛出异常
            case RejectionPolicy::CALLER_RUNS:
                task(); // 由调用线程处理任务
                break;
            case RejectionPolicy::DISCARD:
                break; // 丢弃任务，不做任何处理
            case RejectionPolicy::DISCARD_OLDEST:
                if (!tasks.empty()) {
                    tasks.pop(); // 丢弃最早的任务
                }
                tasks.push(task); // 重新添加当前任务
                break;
        }
    }

    std::vector<std::thread> workers; // 工作线程集合
    std::queue<std::function<void()>> tasks; // 任务队列
    std::mutex queueMutex; // 互斥锁
    std::condition_variable condition; // 条件变量
    bool stop; // 停止标志
    size_t maxQueueSize; // 最大队列大小
    RejectionPolicy rejectionPolicy; // 拒绝策略
    bool allowCoreThreadTimeout; // 是否允许核心线程超时
    std::chrono::milliseconds coreThreadTimeout; // 核心线程超时设置
    std::chrono::steady_clock::time_point lastActiveTime; // 记录上次活跃时间
};

// 单线程执行器
class SingleThreadExecutor : public ThreadPool {
public:
    SingleThreadExecutor(RejectionPolicy policy = RejectionPolicy::ABORT)
        : stop(false), rejectionPolicy(policy) {
        workerThread = std::thread(&SingleThreadExecutor::worker, this); // 启动工作线程
    }

    void enqueue(const std::function<void()>& task) override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            if (tasks.size() >= 1) { // 检查任务队列是否已满
                handleRejection(task); // 处理拒绝逻辑
                return;
            }
            tasks.push(task); // 添加任务到队列
        }
        condition.notify_one(); // 通知工作线程
    }

    void shutdown() override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            stop = true; // 设置停止标志
        }
        condition.notify_all(); // 通知所有线程
        workerThread.join(); // 等待工作线程结束
    }

    std::vector<std::thread::id> shutdownNow() override {
        std::vector<std::thread::id> runningThreads{ workerThread.get_id() }; // 记录正在运行的线程ID
        shutdown(); // 调用平缓关闭
        return runningThreads; // 返回正在运行的线程ID列表
    }

    ~SingleThreadExecutor() {
        shutdown(); // 安全关闭
    }

private:
    void worker() {
        while (true) { // 持续循环
            std::function<void()> task; // 定义任务
            {
                std::unique_lock<std::mutex> lock(this->queueMutex); // 加锁
                this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                if (this->stop && this->tasks.empty()) {
                    return; // 如果停止且队列为空，退出
                }
                task = std::move(this->tasks.front()); // 获取任务
                this->tasks.pop(); // 从队列中移除任务
            }
            try {
                task(); // 执行任务
            } catch (const std::exception& e) {
                std::cerr << "Task execution failed: " << e.what() << std::endl; // 捕获并处理任务异常
            }
        }
    }

    void handleRejection(const std::function<void()>& task) {
        switch (rejectionPolicy) {
            case RejectionPolicy::ABORT:
                throw std::runtime_error("SingleThreadExecutor is busy, task rejected"); // 抛出异常
            case RejectionPolicy::CALLER_RUNS:
                task(); // 由调用线程处理任务
                break;
            case RejectionPolicy::DISCARD:
                break; // 丢弃任务，不做任何处理
            case RejectionPolicy::DISCARD_OLDEST:
                break; // 由于只有一个任务，不能丢弃旧任务
        }
    }

private:
    std::thread workerThread; // 工作线程
    std::queue<std::function<void()>> tasks; // 任务队列
    std::mutex queueMutex; // 互斥锁
    std::condition_variable condition; // 条件变量
    bool stop; // 停止标志
    RejectionPolicy rejectionPolicy; // 拒绝策略
};

// 缓存线程池
class CachedThreadPool : public ThreadPool {
public:
    void enqueue(const std::function<void()>& task) override {
        std::thread(task).detach(); // 创建并分离线程
    }

    void shutdown() override {
        // 缓存线程池通常没有线程管理，这里可以留空
    }

    std::vector<std::thread::id> shutdownNow() override {
        return {}; // 返回空，因为缓存线程池没有正在运行的线程
    }
};

// 定时线程池
class ScheduledThreadPool : public ThreadPool {
public:
    ScheduledThreadPool(size_t numThreads, RejectionPolicy policy = RejectionPolicy::ABORT)
        : stop(false), rejectionPolicy(policy) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ScheduledThreadPool::worker, this); // 启动工作线程
        }
    }

    void enqueue(const std::function<void()>& task) override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            if (tasks.size() >= maxQueueSize) { // 检查任务队列是否已满
                handleRejection(task); // 处理拒绝逻辑
                return;
            }
            tasks.push(task); // 添加任务到队列
        }
        condition.notify_one(); // 通知工作线程
    }

    void shutdown() override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            stop = true; // 设置停止标志
        }
        condition.notify_all(); // 通知所有线程
        for (std::thread &worker : workers) {
            worker.join(); // 等待所有线程结束
        }
    }

    std::vector<std::thread::id> shutdownNow() override {
        std::vector<std::thread::id> runningThreads;
        shutdown(); // 调用平缓关闭
        return runningThreads; // 返回空，因为线程已经关闭
    }

    ~ScheduledThreadPool() {
        shutdown(); // 安全关闭
    }

private:
    void worker() {
        while (true) { // 持续循环
            std::function<void()> task; // 定义任务
            {
                std::unique_lock<std::mutex> lock(this->queueMutex); // 加锁
                this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                if (this->stop && this->tasks.empty()) {
                    return; // 如果停止且队列为空，退出
                }
                task = std::move(this->tasks.front()); // 获取任务
                this->tasks.pop(); // 从队列中移除任务
            }
            try {
                task(); // 执行任务
            } catch (const std::exception& e) {
                std::cerr << "Task execution failed: " << e.what() << std::endl; // 捕获并处理任务异常
            }
        }
    }

    void handleRejection(const std::function<void()>& task) {
        switch (rejectionPolicy) {
            case RejectionPolicy::ABORT:
                throw std::runtime_error("Task queue is full, task rejected"); // 抛出异常
            case RejectionPolicy::CALLER_RUNS:
                task(); // 由调用线程处理任务
                break;
            case RejectionPolicy::DISCARD:
                break; // 丢弃任务，不做任何处理
            case RejectionPolicy::DISCARD_OLDEST:
                if (!tasks.empty()) {
                    tasks.pop(); // 丢弃最早的任务
                }
                tasks.push(task); // 重新添加当前任务
                break;
        }
    }

private:
    std::vector<std::thread> workers; // 工作线程集合
    std::queue<std::function<void()>> tasks; // 任务队列
    std::mutex queueMutex; // 互斥锁
    std::condition_variable condition; // 条件变量
    bool stop; // 停止标志
    RejectionPolicy rejectionPolicy; // 拒绝策略
    size_t maxQueueSize = 5; // 最大队列大小
};

// 有界线程池，实现了核心线程数、最大线程数和最大工作队列数
class BoundedThreadPool : public ThreadPool {
public:
    BoundedThreadPool(size_t coreThreads, size_t maxThreads, size_t maxQueueSize,
                      RejectionPolicy policy = RejectionPolicy::ABORT) 
        : coreThreads(coreThreads), maxThreads(maxThreads), maxQueueSize(maxQueueSize),
          stop(false), rejectionPolicy(policy) {
        // 启动核心线程
        for (size_t i = 0; i < coreThreads; ++i) {
            workers.emplace_back(&BoundedThreadPool::worker, this); // 启动工作线程
        }
    }

    void enqueue(const std::function<void()>& task) override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            // 如果任务数达到核心线程数，开始排队
            if (tasks.size() < coreThreads) {
                tasks.push(task); // 直接添加任务
            } else if (tasks.size() < maxQueueSize) {
                tasks.push(task); // 添加到队列
                condition.notify_one(); // 通知线程可以工作
            } else if (activeThreads < maxThreads) {
                tasks.push(task); // 添加到队列
                condition.notify_one(); // 通知线程可以工作
                // 当任务数量达到最大工作队列数，增加新的线程
                activeThreads++;
                workers.emplace_back(&BoundedThreadPool::worker, this); // 启动新的工作线程
            } else {
                handleRejection(task); // 执行拒绝策略
            }
        }
    }

    void shutdown() override {
        {
            std::unique_lock<std::mutex> lock(queueMutex); // 加锁
            stop = true; // 设置停止标志
        }
        condition.notify_all(); // 通知所有线程
        for (std::thread &worker : workers) {
            worker.join(); // 等待所有线程结束
        }
    }

    std::vector<std::thread::id> shutdownNow() override {
        // 暴力关闭，立即停止
        std::vector<std::thread::id> runningThreads;
        shutdown(); // 调用平缓关闭
        return runningThreads; // 返回正在运行的线程ID列表
    }

    ~BoundedThreadPool() {
        shutdown(); // 安全关闭
    }

private:
    void worker() {
        while (true) { // 持续循环
            std::function<void()> task; // 定义任务
            {
                std::unique_lock<std::mutex> lock(queueMutex); // 加锁
                // 等待条件变量，直到有任务可执行或线程池停止
                this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                if (this->stop && this->tasks.empty()) {
                    return; // 如果停止且队列为空，退出
                }
                task = std::move(this->tasks.front()); // 获取任务
                this->tasks.pop(); // 从队列中移除任务
            }
            try {
                task(); // 执行任务
            } catch (const std::exception& e) {
                std::cerr << "Task execution failed: " << e.what() << std::endl; // 捕获并处理任务异常
            }
        }
    }

    void handleRejection(const std::function<void()>& task) {
        switch (rejectionPolicy) {
            case RejectionPolicy::ABORT:
                throw std::runtime_error("Task queue is full, task rejected"); // 抛出异常
            case RejectionPolicy::CALLER_RUNS:
                task(); // 由调用线程处理任务
                break;
            case RejectionPolicy::DISCARD:
                break; // 丢弃任务，不做任何处理
            case RejectionPolicy::DISCARD_OLDEST:
                if (!tasks.empty()) {
                    tasks.pop(); // 丢弃最早的任务
                }
                tasks.push(task); // 重新添加当前任务
                break;
        }
    }

private:
    size_t coreThreads; // 核心线程数
    size_t maxThreads; // 最大线程数
    size_t maxQueueSize; // 最大工作队列大小
    std::vector<std::thread> workers; // 工作线程集合
    std::queue<std::function<void()>> tasks; // 任务队列
    std::mutex queueMutex; // 互斥锁
    std::condition_variable condition; // 条件变量
    bool stop; // 停止标志
    RejectionPolicy rejectionPolicy; // 拒绝策略
    size_t activeThreads = 0; // 当前活跃线程数
};

#endif // THREADPOOL_H
