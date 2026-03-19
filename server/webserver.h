#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <unordered_map>
#include <fcntl.h>       // fcntl()
#include <unistd.h>      // close()
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "epoller.h"
#include "../log/log.h"
#include "../timer/heaptimer.h"
#include "../pool/sqlconnpool.h"
#include "../pool/threadpool.h"
#include "../pool/sqlconnRAII.h"
#include "../http/httpconn.h"

class WebServer {
public:
    // `port`：服务器监听的端口号（如 8080）。
    // `trigMode`：触发模式（epoll 的 LT 水平触发 或 ET 边缘触发组合）。
    // `timeoutMS`：HTTP 长连接（Keep-Alive）的超时时间，超时会被踢掉。
    // `OptLinger`：是否开启优雅关闭连接（`SO_LINGER` 套接字选项）。
    // 数据库相关（`sqlPort` ~ `connPoolNum`）：MySQL 的端口、账密、库名、连接池大小。
    // `threadNum`：线程池中的工作线程数量。
    // 日志相关（`openLog` ~ `logQueSize`）：是否开启日志、日志级别、异步日志队列容量。
    WebServer(
        int port, int trigMode, int timeoutMS, bool OptLinger,
        int sqlPort, const char* sqlUser, const char* sqlPwd,
        const char* dbName, int connPoolNum, int threadNum,
        bool openLog, int logLevel, int logQueSize
    );

    ~WebServer();

    // 服务器的主循环，调用 `epoll_wait` 阻塞等待事件发生，然后进行分发。
    void start();

private:
    // 创建监听套接字（`socket()`），绑定端口（`bind()`），并开始监听（`listen()`）。成功返回 true。
    bool InitSocket_();

    // 根据传入的 `trigMode`，解析并设置 `listenEvent_` (监听事件的触发模式) 和 `connEvent_` (普通连接的触发模式)。
    void InitEventMode_(int trigMode);

    // 初始化并注册新的客户端连接。
    void AddClient_(int fd, sockaddr_in addr);
    // 处理新的客户端连接请求。
    void DealListen_();
    // 处理客户端套接字的写事件。
    void DealWrite_(HttpConn* client);
    // 处理客户端套接字的读事件。
    void DealRead_(HttpConn* client);

    // 发送错误信息
    void SendError_(int fd, const char* info);
    // 延长定时器的时间。
    void ExtentTime_(HttpConn* client);
    // 关闭连接。
    void CloseConn_(HttpConn* client);

    // 执行真正的读数据和触发业务处理逻辑。
    void OnRead_(HttpConn* client);
    // 执行真正的写数据，将响应发回给客户端。
    void OnWrite_(HttpConn* client);
    // 服务器的“大脑”，处理 HTTP 业务逻辑（解析请求 + 生成响应）。
    void OnProcess_(HttpConn* client);

    // 定义服务器支持的最大文件描述符数量（最大并发数）。
    static const int MAX_FD = 65536;
    // 利用 `fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK)` 将 Socket 设置为非阻塞。
    static int SetFdNonblock(int fd);

    int port_;         // 服务器监听的端口
    bool openLinger_;  // 是否开启 SO_LINGER（控制是close(fd) 立即丢弃缓冲区数据还是等待发送完毕）
    int timeoutMS_;    // 毫秒级超时时间
    bool isClose_;     // 服务器是否已经关闭的标志位（控制 Start() 里的 while 循环）
    int listenFd_;     // 主监听文件描述符（服务器用于 accept 新连接的 socket）
    char* srcDir_;     // 服务器静态资源的根目录路径

    uint32_t listenEvent_;  // 监听 socket 在 epoll 中注册的事件类型
    uint32_t connEvent_;    // 客户端连接 socket 在 epoll 中注册的事件类型

    std::unique_ptr<HeapTimer> timer_;
    std::unique_ptr<ThreadPool> threadpool_;
    std::unique_ptr<Epoller> epoller_;

    // 这是保存客户端连接状态的核心数据结构。
    std::unordered_map<int, HttpConn> users_;
};

#endif