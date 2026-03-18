#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h> // epoll_ctl()
#include <fcntl.h>     // fcntl()
#include <unistd.h>    // close()
#include <assert.h>    // assert()
#include <vector>
#include <errno.h>

class Epoller {
public:
    // `explicit` 关键字：禁止编译器进行隐式的类型转换，强制开发者必须显式调用构造函数
    // maxEvent指定单次 `epoll_wait` 最多能带回多少个就绪事件，默认分配 1024 个空间
    // 这不是限制你的服务器只能连 1024 个客户端
    // 而是指“一次系统调用最多拿回 1024 个结果”，拿不完下一次循环接着拿。
    explicit Epoller(int maxEvent = 1024);
    ~Epoller();

    // 把一个新的文件描述符 `fd`（比如刚 accept 的客户端连接）挂载到内核的 epoll 红黑树上。
    bool AddFd(int fd, uint32_t events);
    // 修改已经挂载的 `fd` 的监听事件。
    bool ModFd(int fd, uint32_t events);
    // 当客户端断开连接时，将其从 epoll 树上摘除，不再监听。
    bool DelFd(int fd);

    // 封装了 `epoll_wait`，挂起当前线程，等待被监听的 fd 发生事件。
    // `timeoutMs = -1`：超时时间（毫秒）。默认 -1 表示死等（一直阻塞直到有事件发生）。
    // 如果传入具体数字（比如 10000），则最多等 10 秒，没事件也返回 0。
    // 返回活跃（有事件发生）的连接个数（通常表示为 `n`）。
    int Wait(int timeoutMs = -1);
    // 获取第i个发生事件的fd
    int GetEventFd(size_t i) const;
    // 获取第i个发生事件的具体类型
    // 返回的是内核传递的事件位掩码
    uint32_t GetEvents(size_t i) const;

private:
    // epoll实例的操作句柄，用于对epoll实例进行各种操作
    int epollFd_;  
    // 事件数组，是每次调用 `Wait()` 时，作为参数传给内核，让内核把当时已经活跃的连接信息写入到这个数组里。
    std::vector<struct epoll_event> events_;  
};

#endif