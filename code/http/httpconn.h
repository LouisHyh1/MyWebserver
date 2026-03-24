#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <sys/types.h>
#include <sys/uio.h>     // readv/writev
#include <arpa/inet.h>   // sockaddr_in
#include <stdlib.h>      // atoi()
#include <errno.h>   

#include "../log/log.h"
#include "../pool/sqlconnRAII.h"
#include "../buffer/buffer.h"
#include "httprequest.h"
#include "httpresponse.h"
#include "ConcurrentAlloc.h"

class HttpConn {
public:
    HttpConn();
    ~HttpConn();

    // 初始化连接。传入刚刚 `accept` 成功产生的新 Socket 文件描述符（`sockFd`）和客户端地址结构体（`addr`）。
    void init(int sockFd, const sockaddr_in& addr);

    // 处理 Socket 上的读写事件。返回值 `ssize_t` 表示实际读写的字节数。
    // 必须立即把当时的 `errno` 取出来保存到传入的指针里
    ssize_t read(int* saveErrno);
    ssize_t write(int* saveErrno);

    // 主动关闭这个 HTTP 连接，释放绑定的系统 Socket 资源。
    void Close();

    // 用于获取当前连接的文件描述符、端口号、IP 字符串和地址结构体。
    int GetFd() const;
    int GetPort() const;
    const char* GetIP() const;
    sockaddr_in GetAddr() const;

    // 当 `read()` 将客户端的报文读取完毕后，调用此函数对报文进行解析，并生成准备发送的响应报文。
    // 返回 `true` 表示处理成功并已准备好响应，可以监听写事件；`false` 表示报文不完整或解析出错。
    bool process();

    // 计算当前还有多少字节的数据需要发送给客户端。
    int ToWriteBytes() {
        return iov_[0].iov_len + iov_[1].iov_len;
    }

    // 判断当前 HTTP 连接是否是“长连接”（Keep-Alive）。
    bool IsKeepAlive() const {
        return request_.IsKeepAlive();
    }

    // ==========================================
    // 为 HttpConn 专属重载 operator new 和 delete
    // ==========================================
    static void* operator new(size_t size) {
        // 调用你的定长内存池/并发内存池
        return ConcurrentAlloc(size); 
    }

    static void operator delete(void* ptr) {
        if (ptr == nullptr) return;
        ConcurrentFree(ptr);
    }
    
    // 如果有对象数组分配需求（虽然一般不会 delete[] HttpConn），可以顺手重载
    static void* operator new[](size_t size) {
        return ConcurrentAlloc(size);
    }

    static void operator delete[](void* ptr) {
        if (ptr == nullptr) return;
        ConcurrentFree(ptr);
    }

    static bool isET;  // 服务器是否工作在 Epoll 的边缘触发（Edge Trigger）模式。
    static const char* srcDir;  // 服务器托管静态文件的根目录路径（比如 `/var/www/html`）。
    static std::atomic<int> userCount;  // 当前服务器同时在线的连接总数。
    // 使用 C++11 提供的原子变量（Atomic），无需加锁就能保证计数的线程安全

private:
    int fd_;  // Socket 文件描述符。操作系统通过这个整型数字来识别底层的网络连接。
    struct sockaddr_in addr_;  // 保存对客户端的 IP 和端口信息。
    bool isClose_;  // 状态标志位。标识当前连接是否已经关闭。

    int iovCnt_;  // 记录当前使用了几块内存

    // struct iovec {
    //     void  *iov_base;    /* 内存块的起始地址 (指针) */
    //     size_t iov_len;     /* 这块内存的长度 (字节数) */
    // };
    // `iov_[0]` 指向响应头所在的内存。
    // `iov_[1]` 指向通过 `mmap` 映射到内存中的静态文件首地址。
    struct iovec iov_[2];

    Buffer readBuff_;   // 读缓冲区
    Buffer writeBuff_;  // 写缓冲区

    // `request_` 对象：专门负责把 `readBuff_` 里的文本数据，解析成结构化的数据
    HttpRequest request_;
    // `response_` 对象：专门负责根据客户端的请求，去磁盘寻找对应文件
    // 生成 HTTP 状态码（200 OK, 404 Not Found）并组装响应头存入 `writeBuff_` 中。
    HttpResponse response_;
};

#endif