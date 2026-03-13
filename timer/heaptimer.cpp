#include "heaptimer.h"

// 交换堆数组中的两个节点。
void HeapTimer::SwapNode_(size_t i, size_t j) {
    assert(i >= 0 && i < heap_.size());
    assert(j >= 0 && j < heap_.size());
    // 交换 `vector` 中 `i` 和 `j` 位置的 `TimerNode` 结构体内容。
    std::swap(heap_[i], heap_[j]);
    // 节点在数组中的位置变了，我们必须去哈希表 `ref_` 里
    // 把这两个定时器 ID 对应的新数组下标更新一下。
    ref_[heap_[i].id] = i;
    ref_[heap_[j].id] = j;
}

// 向上调整（Sift Up）节点 `i` 的位置，使其保持最小堆的性质。
void HeapTimer::siftup_(size_t i) {
    assert(i >= 0 && i < heap_.size());
    // 父节点下标
    size_t j = (i - 1) / 2;
    // 只要没到树的根节点之前，就一直循环。
    while (j >= 0) {
        // 如果父节点（j）的过期时间 已经早于 当前节点（i）
        // 说明当前节点的位置刚刚好，不需要再往上浮动了，直接 `break` 结束。
        if (heap_[j] < heap_[i]) break;
        // 否则，父节点比当前节点大，破坏了最小堆（父节点必须最小），所以将它们交换。
        SwapNode_(i, j);
        // 将当前节点上移到父节点的位置，继续下一轮循环判断。
        i = j;
        j = (i - 1) / 2;
    }
}

// 向下调整（Sift Down）节点 `i` 的位置，使其保持最小堆的性质。
// `index` 是当前下沉的节点位置，`n` 是堆的总大小。
bool HeapTimer::siftdown_(size_t index, size_t n) {
    assert(index >= 0 && index < heap_.size());
    assert(n >= 0 && n <= heap_.size());
    size_t i = index;
    // 节点 `i` 的左孩子下标
    size_t j = i * 2 + 1;
    // 只要左子节点还在堆的范围内，就说明它有子节点，可以尝试下沉。
    while (j < n) {
        // `j+1` 是右子节点。如果右子节点存在，且右子节点比左子节点更早过期，那就让 `j` 指向右子节点。
        if (j + 1 < n && heap_[j + 1] < heap_[j]) j++;
        // 如果当前节点（i）比它最小的孩子（j）还要小，说明它当前位置很完美，停止下沉
        if (heap_[i] < heap_[j]) break;
        // 否则，把它和最小的孩子交换，然后视点下移，继续下一轮判断。
        SwapNode_(i, j);
        i = j;
        j = i * 2 + 1;
    }
    // 返回一个布尔值，表示该节点是否真的发生了下沉
    return i > index;
}

// 添加一个新的定时任务。如果 `id` 已经存在，则更新它的超时时间和回调函数。
// `id` (标识符)，`timeOut` (多长时间后超时，比如 5000 毫秒)，`cb` (绑定的回调函数)。
void HeapTimer::add(int id, int timeout, const TimeoutCallBack& cb) {
    assert(id >= 0);
    size_t i;  // 新节点下标
    // 如果是新节点
    if (ref_.count(id) == 0) {
        // 新节点将放在数组最末尾。
        i = heap_.size();
        // 哈希表记录这个新 FD 的下标。
        ref_[id] = i;
        // 初始化构造一个 `TimerNode` 塞入数组
        heap_.emplace_back(id, Clock::now() + MS(timeout), cb);
        // 新节点放在了最后，可能比它父节点还小，所以调用 `siftup_` 尝试把它向上浮动到正确位置。
        siftup_(i);
    }
    else {
        // 直接从哈希表拿到下标
        i = ref_[id];
        // 修改其过期时间和回调函数。
        heap_[i].expires = Clock::now() + MS(timeout);
        heap_[i].cb = cb;
        // 先尝试让它向下沉 (`siftdown_`)。如果 `siftdown_` 返回 `false`，说明它根本没下沉（它比俩孩子都小）
        // 那它就有可能是变小了，于是再尝试让它向上浮 (`siftup_`)。
        // 这样保证了无论时间怎么改，最多只执行一次调整。
        if (!siftdown_(i, heap_.size())) {
            siftup_(i);
        }
    }
}

// 手动触发指定 `id` 的定时器回调函数，并在堆中删除该定时器。
void HeapTimer::doWork(int id) {
    if (heap_.empty() || ref_.count(id) == 0) return;
    size_t i = ref_[id];
    TimerNode node = heap_[i];
    node.cb();
    del_(i);
}

// 删除数组 `heap_` 中索引为 `i` 的节点，并保持最小堆的性质不被破坏。
// 在数组表示的堆中，删除中间的元素是很麻烦的。
// 标准做法是：把要删除的元素和数组最后一个元素交换位置，然后删除数组最后一个元素。
void HeapTimer::del_(size_t index) {
    assert(!heap_.empty() && index >= 0 && index < heap_.size());
    // 记录当前元素下标
    size_t i = index;
    // 获取最后一个元素的下标。
    size_t n = heap_.size() - 1;
    assert(i <= n);
    if (i < n) {
        // 如果不是要删除最后一个元素，就把它俩交换。
        SwapNode_(i, n);
        // 交换完毕后，原来在队尾的元素来到了下标 `i` 的位置。
        // 这个外来户可能破坏了堆的结构，所以同样使用“先尝试下沉，若没下沉再尝试上浮”的组合拳，把它安排到正确位置。
        // 注意此时传入的有效长度是 `n`，把队尾元素排除在堆外了。
        // siftdown中的n为堆数组长度，此处的n为堆数组长度 - 1，因此队尾元素被排除
        if (!siftdown_(i, n)) {
            siftup_(i);
        }
    }
    // 最后，安全地把在队尾的（本该被删除的）元素从哈希表和数组中剔除。
    ref_.erase(heap_.back().id);
    heap_.pop_back();
}

// 调整已存在的定时器的过期时间。
// 客户端发来一个心跳包（Keep-Alive），说明它还活着，我们需要把它的超时时间往后推迟。
void HeapTimer::adjust(int id, int timeout) {
    assert(!heap_.empty() && ref_.count(id) > 0);
    heap_[ref_[id]].expires = Clock::now() + MS(timeout);
    // 由于超时时间只能往后推（值变大），所以它在最小堆里只能往下沉。
    // 因此修改完时间后，直接调用 `siftdown_` 即可。
    siftdown_(ref_[id], heap_.size());
}

// 处理超时的定时器。
void HeapTimer::tick() {
    if (heap_.empty()) return;
    // 循环检查，因为可能同一时刻有多个定时器到期。
    while (!heap_.empty()) {
        // `front()` 就是 `heap_[0]`，永远是所有定时器中最快要到期的那一个。
        TimerNode node = heap_.front();

        // `node.expires - Clock::now()`：计算过期时间减去当前系统时间。
        // duration_cast 的作用：它是专门用来在不同的 duration（时间段）类之间进行安全转换的工具。
        // 在这里，它把刚才减出来的纳秒级别的时间段，安全地向下转换成了毫秒（MS）级别的时间段。

        // 经过 duration_cast<MS> 处理后，我们现在手里的依然是一个对象（类型为 std::chrono::milliseconds）。
        // count() 函数的作用就是打破包装，把 duration 对象内部存储的那个整数值原封不动地掏出来。

        // 如果这个值大于0，说明堆顶元素还没到期。
        // 因为这是最小堆，如果最小的都没到期，意味着剩下的所有定时器都没有到期！直接 `break` 退出循环
        if (std::chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0) {
            break;
        }
        // 如果到期了，执行回调函数（比如关闭超时的连接），然后 `pop()`。
        node.cb();
        pop();
    }
}

// 弹出（删除）堆顶部的定时器。
void HeapTimer::pop() {
    assert(!heap_.empty());
    // 删除堆顶元素，并会自动触发下沉调整，把新的最小元素顶上来到 `heap_[0]`。
    del_(0);
}

// 计算距离下一个定时器触发，还剩下多少毫秒。

// 如果服务器当前没有网络请求进来，把 `GetNextTick()` 的返回值传给 `epoll_wait`。
// 这样一来，服务器就会安静地沉睡。直到刚好有一个定时器到期的时间点，内核会准时把服务器唤醒，去处理超时事件！
// 这是高性能并发服务器标准的 Reactor 模式定时器处理手法。
int HeapTimer::GetNextTick() {
    tick();  // 先把当前已经超时的定时器全都处理掉。
    size_t res = -1;
    if (!heap_.empty()) {
        // 获取堆顶元素（下一个要超时的）距离现在的毫秒数。
        res = std::chrono::duration_cast<MS>(heap_.front().expires - Clock::now()).count();
        // 如果在计算的这两微秒内，它刚好过期了，算出来是负数。
        // `epoll_wait` 传负数代表永久阻塞，这就惨了，所以强行置为 0，让 `epoll_wait` 立即返回不阻塞。
        if (res < 0) res = 0;
    }
    return res;
}