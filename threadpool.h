#pragma once
#include <assert.h>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
#include <memory>

class ThreadPool {
public:
    // `explicit`：禁止隐式类型转换，强制要求调用构造函数
    // `size_t threadCount = 8`：默认创建 8 个工作线程。
    // 初始化列表。在堆内存上动态分配一个 `Pool` 实例，并交由 `pool_` 智能指针管理。
    explicit ThreadPool(size_t threadCount = 8) : pool_(std::make_shared<Pool>()) {
        // 确保线程池至少有 1 个线程
        assert(threadCount > 0);
        for (size_t i = 0; i < threadCount; i++) {
            // 实例化一个线程对象，参数是一个 Lambda 表达式（代表线程真正的执行体）。
            // 它把类成员 `pool_` 拷贝了一份给 Lambda 内部的局部变量 `pool`。
            // 极其重要的作用：这一步使得 `shared_ptr` 的引用计数 `+1`。
            // 每个独立的后台线程都持有这块内存的一份所有权。
            std::thread([pool = pool_] {
                // 线程启动后，第一时间利用 `unique_lock` 对 `mtx` 进行加锁。
                // `unique_lock` 支持随时手动解锁和加锁，是配合条件变量使用的唯一选择。
                std::unique_lock<std::mutex> locker(pool->mtx);
                // 进入无限死循环，作为线程监听的常驻后台。
                while (true) {
                    // 首先检查任务队列是否为空。如果不为空：
                    if (!pool->tasks.empty()) {
                        // 将pool->tasks.front()转换为右值（将亡值），因为马上就要让他pop了
                        // 这样可以避免深拷贝的开销
                        auto task = std::move(pool->tasks.front());
                        pool->tasks.pop();
                        // 取到任务后，立刻解锁！这一步极其关键
                        // 如果不解锁，当前线程在执行耗时任务时，其他线程连排队取任务的资格都没有
                        // 线程池就退化成了串行单线程。
                        locker.unlock();
                        // 实际执行任务主体。此时是无锁状态，多个线程可以真正并行执行各自的任务。
                        task();
                        // 任务执行完毕。为了安全地进入下一轮 `while` 循环去读取 `pool->tasks.empty()`，必须重新加锁。
                        locker.lock();
                    }
                    // 如果队列为空，且线程池被标记为关闭，说明“既没活干，又收到了下班通知”，立刻 `break` 跳出死循环。
                    else if (pool->isClosed) break;
                    // 如果队列为空，且没收到关闭通知，说明“暂时没活干”。调用 `wait(locker)` 让当前线程休眠。
                    // `wait(locker)` 的底层机制：
                    // 当前线程休眠的同时，会自动释放 `locker` 所持有的互斥锁，从而允许其他线程投递任务。
                    // 当其他线程调用 `notify_one()` 或 `notify_all()` 时，该线程会被唤醒。
                    // 唤醒后，`wait()` 函数会在返回前自动重新获取互斥锁，然后继续执行下一轮 `while(true)` 循环判断。
                    else pool->cond.wait(locker);
                }
            // 极为核心的一步！将 `std::thread` 对象与底层的系统级线程分离。
            // 当 C++ 中 `std::thread` 的局部对象在 `for` 循环结束被销毁时，系统级线程不受影响，继续在后台执行。
            }).detach();  
        }
    }

    ThreadPool() = default;  // 默认无参构造

    ThreadPool(ThreadPool&&) = default;  // 默认移动构造

    ~ThreadPool() {
        // 检查智能指针是否有效。因为如果当前线程池对象之前被 `std::move` 过
        // `pool_` 会变成空指针，直接操作会引发段错误（Crash）。
        if (static_cast<bool>(pool_)) {
            // 构建一个局部作用域
            {
                // 这一步必须加锁，因为后台的线程可能正在读取 `isClosed` 的状态。
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            // 作用：调用 `notify_all()` 唤醒所有正在 `cond.wait(locker)` 处睡眠的工作线程。
            pool_->cond.notify_all();
        }
    }

    // 向线程池添加新任务的通用 API。
    template<typename F>
    void AddTask(F&& task) {  // 这里的 `&&` 并不是右值引用，而是配合模板使用的 万能引用
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            // 完美转发。它能保持 `task` 原本的左右值属性。
            // 如果外部传入的是右值，就继续当右值 `move` 给队列；如果是左值，就安全地进行 Copy。
            pool_->tasks.emplace(std::forward<F>(task));
        }
        // 任务安全入队并解锁后，调用 `notify_one()`。
        pool_->cond.notify_one();
    }
private:
    // 共享状态结构体
    struct Pool {
        // 互斥锁。保证在同一时刻，只有一个线程能访问任务队列和关闭标志。
        std::mutex mtx;
        // 条件变量。它的作用是让没有任务可做的线程“睡觉”，而不是空转消耗 CPU
        std::condition_variable cond;
        // 线程池关闭标志位
        bool isClosed;
        // 任务队列
        // `std::function<void()>` 意味着它可以存储任何 没有参数、没有返回值的可调用对象。
        std::queue<std::function<void()>> tasks;
    };
    std::shared_ptr<Pool> pool_;
};