#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H

#include <mutex>
#include <deque>
#include <condition_variable>
#include <sys/time.h>

template<class T>
class BlockDeque {
public:
    explicit BlockDeque(size_t MaxCapacity = 1000);

    ~BlockDeque();

    void clear();

    bool empty();

    bool full();

    void Close();

    size_t size();

    size_t capacity();

    T front();

    T back();

    void push_back(const T& item);

    void push_front(const T& item);

    bool pop(T& item);

    bool pop(T& item, int timeout);

    void flush();
private:
    // 底层的双端队列，真正用来存数据的地方。
    std::deque<T> deq_;

    // 队列的最大容量。
    size_t capacity_;

    // 互斥锁。由于会有多个线程同时读写 `deq_`，必须用锁保证同一时刻只有一个线程能操作底层队列。
    std::mutex mtx_;

    // 布尔标志位。用于标记队列是否已经关闭。
    bool isClose_;

    // 消费者的条件变量。当队列为空时，消费者线程在这里阻塞等待；当生产者放入数据时，通过这个变量唤醒消费者。
    std::condition_variable condConsumer_;

    // 生产者的条件变量。当队列为满时，生产者线程在这里阻塞等待；当消费者拿走数据时，通过这个变量唤醒生产者。
    std::condition_variable condProducer_;
};

// 构造函数
template<class T>
BlockDeque<T>::BlockDeque(size_t MaxCapacity) : capacity_(MaxCapacity) {
    assert(MaxCapacity > 0);
    isClose_ = false;
}

// 析构函数
template<class T>
BlockDeque<T>::~BlockDeque() {
    Close();
}

// 析构函数中的释放资源函数
template<class T>
void BlockDeque<T>::Close() {
    {
        std::lock_guard<std::mutex> locker(mtx_);
        deq_.clear();
        isClose_ = true;
    }
    // 唤醒线程为什么不放在解锁前
    // 被唤醒的线程在从 wait() 返回之前，必须重新获取互斥锁 mtx_。
    // 但是，此时当前线程还没有退出大括号，因此 mtx_ 锁还没有被释放。
    // 那些刚被唤醒的线程发现锁被占用，立刻又被迫进入阻塞状态等待锁释放
    // 会引发多余的线程上下文切换，造成性能浪费。
    condProducer_.notify_all();
    condConsumer_.notify_all();
}

template<class T>
void BlockDeque<T>::clear() {
    std::lock_guard<std::mutex> locker(mtx_);
    deq_.clear();
}

template<class T>
T BlockDeque<T>::front() {
    // 为什么只读操作（如 `size()`）也要加锁？
    // 因为在多线程环境下，当你读取 `size` 的同时
    // 可能另一个线程正在 `push`，导致底层容器的内部指针状态不一致，发生段错误。
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.front();
}

template<class T>
T BlockDeque<T>::back() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.back();
}

template<class T>
size_t BlockDeque<T>::size() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size();
}

template<class T>
size_t BlockDeque<T>::capacity() {
    std::lock_guard<std::mutex> locker(mtx_);
    return capacity_;
}

template<class T>
void BlockDeque<T>::flush() {
    // 唤醒一个正在等待的消费者。
    condConsumer_.notify_one();
}

// 生产者：向队列添加数据
template<class T>
void BlockDeque<T>::push_back(const T& item) {
    // 后面的 `wait` 函数需要能够主动解锁和重新加锁的功能，所以用unique_lock
    std::unique_lock<std::mutex> locker(mtx_);
    // 为什么用 `while` 而不是 `if`？
    // 处于 `wait` 状态的线程也有极小概率自己醒过来。
    // 如果用 `if`，线程假醒后会直接往下执行，把数据塞进已经满了的队列，导致溢出崩溃。
    // 用 `while` 的话，醒来后会再次检查条件，如果还是满的，就继续睡回去。
    while (deq_.size() >= capacity_) {
        // 1. 当前线程释放 `mtx_` 锁
        // 2. 当前线程进入休眠，交出 CPU 控制权，进入阻塞等待队列。
        // 3. 当被消费者 `notify` 唤醒时，当前线程自动重新获取 `mtx_` 锁，然后往下执行。
        condProducer_.wait(locker);
    }
    deq_.push_back(item);
    // ，叫醒其中一个消费者来拿数据。
    condConsumer_.notify_one();
}

template<class T>
void BlockDeque<T>::push_front(const T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.size() >= capacity_) {
        condProducer_.wait(locker);
    }
    deq_.push_front(item);
    condConsumer_.notify_one();
}

template<class T>
bool BlockDeque<T>::empty() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.empty();
}

template<class T>
bool BlockDeque<T>::full() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size() >= capacity_;
}

// 消费者：从队列获取数据 (阻塞式)
template<class T>
bool BlockDeque<T>::pop(T& item) {  // 要获取的数据通过引用传递（输出参数）带出来。
    std::unique_lock<std::mutex> locker(mtx_);
    while (deq_.empty()) {  // 依然是为了防虚假唤醒。队列为空时，消费者必须等待。
        condConsumer_.wait(locker);
        // 如果线程在休眠时，系统调用了 `Close()`，
        // `Close()` 里的 `notify_all()` 会把这个线程唤醒。
        // 醒来后发现队列是空的，且已经被要求关闭了，直接返回 `false`，让外部调用线程结束循环退出。
        if (isClose_) return false;
    }
    item = deq_.front();
    deq_.pop_front();
    // 此时叫醒一个可能因为队列满了而在睡觉的生产者
    condProducer_.notify_one();
    return true;
}

// 这个函数比上一个多了一个 `timeout`（超时时间，单位为秒）。
// 防止消费者线程永久死锁等待。
template<class T>
bool BlockDeque<T>::pop(T& item, int timeout) {
    std::unique_lock<std::mutex> locker(mtx_);
    while (deq_.empty()) {
        // 这个 API 允许线程最多睡 `timeout` 秒。
        // `wait_for` 返回一个状态码。
        // 如果返回的是 `timeout`，说明时间到了还没人往队列里塞数据（没被唤醒），那么直接返回 `false`。
        if (condConsumer_.wait_for(locker, std::chrono::seconds(timeout))
            == std::cv_status::timeout) {
            return false;
        }
        if (isClose_) return false;
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

#endif