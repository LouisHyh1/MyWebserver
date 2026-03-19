#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <queue>
#include <unordered_map>
#include <time.h>
#include <algorithm>
#include <arpa/inet.h> 
#include <functional> 
#include <assert.h> 
#include <chrono>
#include "../log/log.h"


// 定义了一个无参数、无返回值 `void()` 的函数包装器。作用：当定时器超时，会触发这个回调函数
typedef std::function<void()> TimeoutCallBack;
// 将 `high_resolution_clock`（当前系统中最高精度的时钟）重命名为 `Clock`。
typedef std::chrono::high_resolution_clock Clock;
// 将毫秒类型 `std::chrono::milliseconds` 简写为 `MS`
typedef std::chrono::milliseconds MS;
// 时间点类型。代表一个绝对的时刻。定时器通过比较当前时刻与 `TimeStamp` 来判断是否超时。
typedef Clock::time_point TimeStamp; 

struct TimerNode {
    // 定时器的唯一标识。在网络服务器中，通常存放的是客户端的 Socket FD
    int id;
    // 过期时间（绝对时间）。如果当前时间大于这个时间，说明超时了。
    TimeStamp expires;
    // 超时后需要执行的回调函数。
    TimeoutCallBack cb;
    // 过期时间越早，节点越“小”
    bool operator<(const TimerNode& t) {
        return expires < t.expires;
    }
};

class HeapTimer {
public:
    // 提前为底层数组 `heap_` 分配 64 个元素的空间。
    HeapTimer() { heap_.reserve(64); }

    // 调用内部的 `clear()` 方法，释放所有资源。
    ~HeapTimer() { clear(); }

    // 调整已存在的定时器的过期时间。
    // 客户端发来一个心跳包（Keep-Alive），说明它还活着，我们需要把它的超时时间往后推迟。
    void adjust(int id, int newExpires);

    // 添加一个新的定时任务。如果 `id` 已经存在，则更新它的超时时间和回调函数。
    // `id` (标识符)，`timeOut` (多长时间后超时，比如 5000 毫秒)，`cb` (绑定的回调函数)。
    void add(int id, int timeOut, const TimeoutCallBack& cb);

    // 手动触发指定 `id` 的定时器回调函数，并在堆中删除该定时器。
    // 客户端主动发起了断开连接请求，服务器不需要再等它超时了，直接立刻清理资源。
    void doWork(int id);

    // 清空整个定时器（清空堆数组和哈希表）。
    void clear();

    // 处理超时的定时器。
    void tick();

    // 弹出（删除）堆顶部的定时器。
    void pop();

    // 计算距离下一个定时器触发，还剩下多少毫秒。
    int GetNextTick();

private:
    // 删除数组 `heap_` 中索引为 `i` 的节点，并保持最小堆的性质不被破坏。
    void del_(size_t i);

    // 向上调整（Sift Up）节点 `i` 的位置，使其保持最小堆的性质。
    void siftup_(size_t i);

    // 向下调整（Sift Down）节点 `i` 的位置，使其保持最小堆的性质。
    // `index` 是当前下沉的节点位置，`n` 是堆的总大小。
    // 返回布尔值通常表示是否真正发生了下沉（位置是否改变）。
    bool siftdown_(size_t index, size_t n);

    // 交换堆数组中的两个节点。
    // 这里不仅仅是交换 `heap_[i]` 和 `heap_[j]`
    // 交换数组元素的同时，必须同步更新哈希表中这两个 `id` 所对应的索引。
    void SwapNode_(size_t i, size_t j);

    // 使用动态数组来存储“最小堆”这棵完全二叉树。
    std::vector<TimerNode> heap_;  
    // 哈希表，用于快速定位节点在堆中的索引。
    std::unordered_map<int, size_t> ref_;  
};

#endif