#include <iostream>
#include <thread>
#include <chrono>

// #include "Task.h"
#include "ThreadPoolExecutor.h"
//#include "RejectTaskHandle.h"

class SimpleRejectTaskHandler : public RejectTaskHandler {
public:
    void reject(const std::function<void()>& task) override {
        std::cout << "Task rejected." << std::endl;
    }
};

int main() {
    SimpleRejectTaskHandler rejectHandler;  // 自定义拒绝策略
    ThreadPool pool(5, 10, 10, &rejectHandler); 

    // 提交任务
    for (int i = 0; i < 15; ++i) {  // 提交15个任务
        pool.submit([i]() {
            std::cout << "Task " << i << " is running on thread " << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1)); // 模拟任务执行1秒
        }); // 使用 Lambda 提交任务
    }

    // 等待一段时间以确保所有任务都有机会执行
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 尝试关闭线程池
    pool.shutdown();  // 或者使用 pool.shutdownNow(); 强制关闭

    return 0;
}
