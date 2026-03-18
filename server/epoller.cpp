#include "epoller.h"

// 调用 Linux 系统函数 `epoll_create(512)`。内核会在底层创建 epoll 对象（包含一棵红黑树和一个双向就绪链表）。
// 内核要求epoll_create的参数必须大于 0，所以随便传个 512（或者 5、100）都可以。
// 返回值会被赋给私有变量 `epollFd_` 作为后续操作的唯一句柄。
// 调用 `std::vector` 的构造函数，直接在堆内存中分配了大小为 `maxEvent`（默认 1024）的数组空间。
Epoller::Epoller(int maxEvent) : epollFd_(epoll_create(512)), events_(maxEvent) {
    // 如果 `epoll_create` 失败（比如系统文件描述符耗尽），会返回 -1
    // 并确保接收数组被成功分配了空间。
    assert(epollFd_ >= 0 && events_.size() > 0);
}

Epoller::~Epoller() {
    // 调用 `close()` 关闭内核中的 epoll 实例，释放底层的红黑树和链表内存，确保绝对不会发生资源泄漏。
    close(epollFd_);
}

// 把一个新的文件描述符 `fd`（比如刚 accept 的客户端连接）挂载到内核的 epoll 红黑树上。
bool Epoller::AddFd(int fd, uint32_t events) {
    if (fd < 0) return false;

    // struct epoll_event {
    //     uint32_t     events;      /* Epoll events (需要监听的事件类型) */
    //     epoll_data_t data;        /* User data variable (用户数据) */
    // };

    // typedef union epoll_data {
    //     void        *ptr;
    //     int          fd;
    //     uint32_t     u32;
    //     uint64_t     u64;
    // } epoll_data_t;

    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    // op:你要执行什么操作  fd:你想要监听的那个具体的目标文件描述符  event:监听事件的指针
    epoll_event ev = { 0 };
    // 告诉内核，“当这个事件发生时，请把这个 fd 原封不动地通过就绪队列还给我，以便我知道是谁触发了事件”。
    ev.data.fd = fd;
    // 告诉内核要监听的具体事件（比如 `EPOLLIN` 读、`EPOLLOUT` 写、`EPOLLET` 边缘触发）。
    ev.events = events;
    // 把这个 `fd` 作为新的节点，插入到 epoll 的红黑树里”。成功时返回 0
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
}

// 修改已经挂载的 `fd` 的监听事件。
bool Epoller::ModFd(int fd, uint32_t events) {
    if (fd < 0) return false;
    epoll_event ev = { 0 };
    ev.data.fd = fd;
    ev.events = events;
    // 传入了 `EPOLL_CTL_MOD` 宏。内核会在红黑树中找到这个 `fd` 对应的节点，修改它的监听事件。
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

// 当客户端断开连接时，将其从 epoll 树上摘除，不再监听。
bool Epoller::DelFd(int fd) {
    if (fd < 0) return false;
    epoll_event ev = { 0 };
    // 传入 `EPOLL_CTL_DEL`，让内核从红黑树中把这个 `fd` 对应的节点彻底删除。
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, &ev);
}

// 封装了 `epoll_wait`，挂起当前线程，等待被监听的 fd 发生事件。
int Epoller::Wait(int timeoutMs) {
    // `epoll_wait`：系统调用。让线程去睡觉（阻塞），直到有 fd 发生了我们监听的事件，或者超时时间 `timeoutMs` 到了，线程才会被唤醒。
    // `&events_[0]`：事件数组头指针
    // events_.size(): 数组能容纳的最大事件数量
    // timeoutMs: 超时时间
    // 返回实际发生事件的 fd 数量。
    return epoll_wait(epollFd_, &events_[0], static_cast<int>(events_.size()), timeoutMs);
}

// 获取第i个发生事件的fd
int Epoller::GetEventFd(size_t i) const {
    assert(i >= 0 && i < events_.size());
    return events_[i].data.fd;
}

// 获取第i个发生事件的具体类型
// 返回一个 32 位的无符号整数（位图）
uint32_t Epoller::GetEvents(size_t i) const {
    assert(i >= 0 && i < events_.size());
    return events_[i].events;
}