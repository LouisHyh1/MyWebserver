#include "webserver.h"
using namespace std;

WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
{
    // 获取当前工作目录的绝对路径，底层会自动 `malloc` 内存（大小最多256字节）赋给 `srcDir_`。
    srcDir_ = getcwd(nullptr, 256);
    assert(srcDir_);
    // 将 `"/resources/"` 拼接到当前路径后。
    strncat(srcDir_, "/resources/", 16);

    // `HttpConn` 是处理单个 HTTP 连接的类。这里初始化它的静态成员变量。
    // 所有的 HTTP 连接共享同一个静态资源目录 `srcDir`，且需要一个全局的 `userCount` 来统计当前在线的连接数。
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;

    // 使用单例模式 (`Instance()`) 获取全局唯一的数据库连接池，并初始化。
    SqlConnPool::Instance()->Init("127.0.0.1", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    // 解析传入的 `trigMode`，设置监听 Socket 和 连接 Socket 的 Epoll 触发模式
    InitEventMode_(trigMode);
    // 创建 `listenFd_`，绑定端口并 `listen`。
    // 如果初始化网络失败，将服务器状态 `isClose_` 设为 true，防止后续启动。
    if (!InitSocket_()) isClose_ = true;

    // 判断服务器配置项 `openLog` 是否为 true（开启日志）。
    if (openLog) {
        // 调用静态方法获取日志类的全局唯一实例
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        // 如果前面绑定端口失败（比如 80 端口被占用了），`isClose_` 会变成 true。
        // 这里检测到 true，就会使用 `LOG_ERROR` 宏打印大写的错误提示，告诉开发者“服务器启动失败了”。
        if (isClose_) {
            LOG_ERROR("========== Server init error!==========");
        }
        else {
            LOG_INFO("========== Server init ==========");
            // `port_`：服务器监听的端口号。
            // `OptLinger` 对应底层 Socket 的 `SO_LINGER` 选项。
            // 如果为 true，表示服务器关闭时，会等待发送缓冲区的数据尽量发送给客户端后再断开（优雅关闭）；
            // 如果为 false，则直接丢弃并发送 RST 包（暴力关闭）。
            LOG_INFO("Port: %d, OpenLinger: %s", port_, OptLinger ? "true" : "false");
            // 打印 Epoll 的触发模式。
            // `listenEvent_ & EPOLLET`：用于检测 `listenEvent_` 这个整数里，有没有包含 `EPOLLET` 这个标志。
            // 如果有（结果非 0），则打印 `"ET"`（边缘触发模式），否则打印 `"LT"`（水平触发模式）。
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                        (listenEvent_ & EPOLLET ? "ET" : "LT"), 
                        (connEvent_ & EPOLLET ? "ET" : "LT"));
            // 打印当前日志级别：方便确认线上环境是不是不小心开成了 Debug 级别
            LOG_INFO("LogSys level: %d", logLevel);
            // 打印静态资源的根目录
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            // 打印池化资源的数量
            // `connPoolNum`：MySQL 数据库连接池里维持了多少个长连接。
            // `threadNum`：工作线程池里有多少个线程在等待处理 HTTP 业务逻辑。
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}   

WebServer::~WebServer() {
    // 关闭服务端的监听 Socket，不再接受新连接。
    close(listenFd_);
    // 改变服务器状态标志，会让主循环退出。
    isClose_ = true;
    // 极其关键！因为前面 `getcwd(nullptr, 256)` 底层调用了 `malloc`
    // 所以这里必须用 `free` 释放内存，否则会导致内存泄漏。
    free(srcDir_);
    // 销毁数据库连接池，断开与 MySQL 的所有 TCP 连接。
    SqlConnPool::Instance()->ClosePool();
}

void WebServer::InitEventMode_(int trigMode) {
    // `EPOLLRDHUP`: 开启这个选项后，如果对端（客户端）断开连接或半关闭
    // 底层会直接触发该事件，无需等到 `read` 返回 0 才判断
    listenEvent_ = EPOLLRDHUP;
    // `EPOLLONESHOT`: 极其重要的多线程设计！ 保证一个 socket 连接在任意时刻，都只会被一个线程处理。
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;

    switch (trigMode) {
    // 0: listen LT, conn LT (默认就是LT)
    case 0 : break;
    // 1: listen LT, conn ET
    case 1 : connEvent_ |= EPOLLET; break;
    // 2: listen ET, conn LT
    case 2 : listenEvent_ |= EPOLLET; break;
    // 3: 全部 ET
    case 3 : listenEvent_ |= EPOLLET; connEvent_ |= EPOLLET; break;
    // 默认：全部 ET
    default : listenEvent_ |= EPOLLET; connEvent_ |= EPOLLET; break;
    }

    // 最后将配置结果通过静态变量赋给 `HttpConn`
    // 让每个 HTTP 连接知道自己在读写时是否需要使用 ET 模式的“循环读写”逻辑。
    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {
    // 决定了 `epoll_wait` 的超时时间。-1 表示如果没有网络事件，线程就永远阻塞（休眠），不消耗 CPU。
    int timeMS = -1;

    if (!isClose_) {
        LOG_INFO("========== Server start ==========");
    }

    // 服务器主循环。
    while (!isClose_) {
        // 如果开启了超时剔除（`timeoutMS_ > 0`），服务器就不能无限阻塞等待网络事件了。
        if (timeoutMS_ > 0) {
            // 会清理掉当前已经超时的连接，并返回距离下一个即将超时的连接还需要多少毫秒。
            // 将这个时间作为 `epoll_wait` 的阻塞时间，确保定时器能极其精准地触发。
            timeMS = timer_->GetNextTick();
        }
        // 这是整个程序最核心的系统调用封装，底层是 `epoll_wait`。
        // 主线程在此阻塞。有网络事件发生或超时，才会往下执行。
        // `eventCnt` 是触发事件的文件描述符数量。
        int eventCnt = epoller_->Wait(timeMS);

        for (int i = 0; i < eventCnt; i++) {
            int fd = epoller_->GetEventFd(i);
            uint32_t events = epoller_->GetEvents(i);
            // 如果发生事件的 `fd` 是服务器监听的 `listenFd_`
            // 说明有新的客户端发起了三次握手，调用 `DealListen_()` 去接收它。
            if (fd == listenFd_) {
                DealListen_();
            }
            // 异常/断开事件： `EPOLLHUP` (挂起), `EPOLLERR` (错误), `EPOLLRDHUP` (对端关闭)。
            // EPOLLRDHUP: 客户端关闭了底层的写端（发送了 FIN 报文）。
            // EPOLLERR: 对应的文件描述符（fd）发生了异步错误。
            // EPOLLHUP: 套接字读写通道均已关闭。
            else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                assert(users_.count(fd) > 0);
                // `users_`: 保存了 `fd` 到客户端连接对象的映射。
                // 如果出错，调用 `CloseConn_` 断开并清理客户端。
                CloseConn_(&users_[fd]);
            }
            // 读事件（EPOLLIN）：客户端发来了 HTTP 请求数据。调用 `DealRead_`。
            else if (events & EPOLLIN) {
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            // 写事件（EPOLLOUT）：操作系统的写缓冲区有空间了，可以向客户端发送响应数据了。调用 `DealWrite_`。
            else if (events & EPOLLOUT) {
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            }
            else {
                // 未知事件，记录错误日志。
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

// 发送错误信息，info为包含具体错误提示信息的 C 风格字符串
// 作用：当服务器过载时，向客户端发送一句提示语（如 "Server busy!"），然后直接调用 `close()` 切断 TCP 连接。
void WebServer::SendError_(int fd, const char* info) {
    assert(fd > 0);
    // `send` 的返回值如果是 大于 0 的数，表示实际拷贝到内核缓冲区的字节数。
    // 如果返回 小于 0（通常是 `-1`），说明发生了错误。
    int ret = send(fd, info, strlen(info), 0);
    if (ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

// 关闭连接。
void WebServer::CloseConn_(HttpConn* client) {
    assert(client != nullptr);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    // 必须先调用 `epoller_->DelFd` 将该 `fd` 从内核的 Epoll 红黑树上摘除，停止监听。
    epoller_->DelFd(client->GetFd());
    // 调用客户端自身的 `Close()` 方法（其内部通常会关闭 socket fd，释放内存，并将 `HttpConn::userCount` 减 1）。
    client->Close();
}

// 初始化并注册新的客户端连接。
void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    // 利用已有的 `users_` 容器，通过 `fd` 索引到对应的 `HttpConn` 对象
    // 并调用它的 `init` 函数（用于重置它的读写缓冲区、解析状态机等）。
    users_[fd].init(fd, addr);
    // 判断服务器是否开启了超时机制（超时时间大于 0 毫秒）。
    if (timeoutMS_ > 0) {
        // 向服务器的定时器管理器中添加一个新的定时节点。
        // 第三个参数是一个回调函数（Callback）。
        // 它的意思是：“当设定的时间到了，如果这个连接还没动静，就请执行这个打包好的函数”。
        // bind` 的作用就是把这个“需要参数的成员函数”提前塞好参数，伪装/打包成一个“不需要参数的普通函数”交给定时器。
        // 因为定时器节点通常只需要一个形如 `void()`（无参数无返回值）的回调函数
        // 参数 1：`&WebServer::CloseConn_`:你要绑定的目标函数的函数指针（地址）。
        // 参数 2：`this`: 当前 `WebServer` 对象的实例指针。
        // 参数 3：`&users_[fd]`: 传递给 `CloseConn_` 函数的实际参数。
        timer_->add(fd, timeoutMS_, bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    // 将新客户端的 `fd` 注册到 Epoll 中，监听该客户端的可读事件（EPOLLIN），
    // 附加初始化时设定的 `connEvent_`（包含是否开启 ET，是否开启 ONESHOT）。
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    // 将 socket 设置为非阻塞模式。在使用 Epoll（尤其是 ET 模式）时，socket 必须是非阻塞的
    // 否则 `read` 或 `write` 可能会永久挂起主线程，导致服务器死锁
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

// 处理新的客户端连接请求。
void WebServer::DealListen_() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    // 准备保存客户端 IP 和端口的 `addr` 结构体，进入循环。
    do {
        // `accept`: 从全连接队列拿出一个完成三次握手的 socket。
        // `(struct sockaddr *)&addr`: 客户端地址信息
        // 输出型参数，当 accept 成功拿到了一个连接后，操作系统内核会自动把客户端的 IP 地址和端口号填入这个结构体中
        // `&len`：地址结构体的长度，传入时：告诉内核你准备好的 `addr` 结构体有多大
        // 返回时：内核会把它修改为实际填入 `addr` 结构体的字节数。
        int fd = accept(listenFd_, (struct sockaddr*)&addr, &len);

        // 如果 `fd <= 0`，说明队列已经被掏空了
        // （底层其实是返回 -1，并置 errno 为 `EAGAIN` 或 `EWOULDBLOCK`）
        // 此时直接 `return` 退出函数，等待下一次 epoll 触发。
        if (fd <= 0) { return; }
        // 为了防止服务器资源耗尽（OOM 或达到系统上限），主动拒绝多余的连接。
        else if (HttpConn::userCount >= MAX_FD) {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        // 如果没有达到上限，调用上面的 `AddClient_` 完成客户端的初始化。
        AddClient_(fd, addr);
    } while (listenEvent_ & EPOLLET);
    // 判断如果监听 socket 配置了 ET模式，就继续循环 `accept`；如果是 LT模式，只循环一次就退出
}

// 处理客户端套接字的读事件。
void WebServer::DealRead_(HttpConn* client) {
    // 主线程利用 `std::bind` 将类成员函数 `OnRead_`、当前对象指针 `this` 和参数 `client` 绑定成一个任务（Task）
    // 扔进 `threadpool_`（线程池）的任务队列中。由线程池里的工作线程去执行真正的读操作。
    assert(client != nullptr);  // 1. 断言：确保传入的客户端连接指针不为空，防止空指针异常。
    ExtentTime_(client);  // // 2. 刷新超时时间：客户端有活动了，说明它还活着，重置它的倒计时。
    threadpool_->AddTask(bind(&WebServer::OnRead_, this, client));  // 3. 将具体的读任务投递给线程池
}

// 处理客户端套接字的写事件。
void WebServer::DealWrite_(HttpConn* client) {
    assert(client != nullptr);
    ExtentTime_(client);  // 有写事件，同样说明连接活跃，刷新超时时间。
    threadpool_->AddTask(bind(&WebServer::OnWrite_, this, client));  // 将具体的写任务投递给线程池
}

// 延长定时器的时间。
void WebServer::ExtentTime_(HttpConn* client) {
    assert(client != nullptr);
    // 如果设置的超时时间（毫秒）大于0，说明开启了超时剔除功能
    if (timeoutMS_ > 0) {
        // 调用定时器对象，调整该客户端文件描述符（Fd）对应的定时器
        timer_->adjust(client->GetFd(), timeoutMS_);
    }
}

// 执行真正的读数据和触发业务处理逻辑。
void WebServer::OnRead_(HttpConn* client) {
    assert(client != nullptr);
    int ret = -1;  // 存放实际读写的字节数
    int readErrno = 0;  // 存放read返回的错误码errno

    // 1. 调用 client 自己封装的 read 方法读取数据。
    // 底层一定是一个 while 循环调用 recv/read，直到读空为止（因为是 ET 边缘触发模式）。
    ret = client->read(&readErrno);

    // 2. 错误处理与断开连接的判断
    // ret <= 0 表示读取出错或对方关闭连接。
    // 并且 readErrno != EAGAIN。
    // EAGAIN 是非阻塞 I/O 的特性，表示系统缓冲区暂时没数据可读了，这不是错误，是正常的非阻塞返回。
    if (ret <= 0 && readErrno != EAGAIN) {
        // 真出错了或者客户端正常断开（ret==0），关闭连接释放资源。
        CloseConn_(client);
        return;
    }

    // 3. 读取成功（缓冲区有数据了），进入业务处理逻辑
    OnProcess_(client);
}

void WebServer::OnProcess_(HttpConn* client) {
    // client->process() 会执行 HTTP 请求的解析（请求行、请求头、请求体），并生成 HTTP 响应内容。
    // 如果解析并准备好了响应，返回 true；如果请求包不完整，还需要继续读，返回 false。
    if (client->process()) {
        // 如果处理完毕，准备好发数据了。
        // 将该 Fd 在 epoll 中的监听事件修改为：基础连接事件(connEvent_) + 监听可写事件(EPOLLOUT)。
        // 这样 epoll 就会在 socket 发送缓冲区有空间时，触发 Write 事件。
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
    }
    else {
        // 如果请求还没收完整（例如遇到了大文件长传，一次 read 没读完）。
        // 保持监听可读事件(EPOLLIN)，等剩下的数据到了继续读。
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn* client) {
    assert(client != nullptr);
    int ret = -1;
    int writeErrno = 0;

    // 1. 调用 client 封装的 write 方法发送数据（底层通常是 writev 分散写）
    ret = client->write(&writeErrno);

    // 2. 判断是否所有数据都已经发送完毕
    if (client->ToWriteBytes() == 0) {
        /* 传输完成 */
        // 判断 HTTP 请求头里是否带有 Connection: keep-alive
        if (client->IsKeepAlive()) {
            // 如果是长连接，复位 HTTP 状态机，准备接收该连接的下一个 HTTP 请求
            OnProcess_(client);
            return;
        }
    }
    // 3. 如果没发送完，且底层返回发送失败 (< 0)
    else if (ret < 0) {
        if (writeErrno == EAGAIN) {
            /* 继续传输 */
            // EAGAIN 表示系统缓冲区暂时没数据可读了
            // 说明网卡发送缓冲区满了，塞不进去了。
            // 重新注册 EPOLLOUT 事件，等内核把缓冲区的数据发到网上，腾出空间了，再通知我们继续写。
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }

    // 4. 如果不是长连接，或者发生了不可挽回的写入错误（不是 EAGAIN），直接关闭连接
    CloseConn_(client);
}

bool WebServer::InitSocket_() {
    // 返回值，通常 `0` 表示成功，`< 0` 表示失败。我们用它来接住每一步操作的结果。
    int ret;  
    // 这是一个专用于 IPv4 地址的结构体。
    // `sockaddr_in` 将 IP 和端口分开了，方便我们填入数据。
    // `addr` 变量就是用来存放我们服务器打算监听的 IP 和端口号的。
    struct sockaddr_in addr;
    // 0 ~ 1023 被称为知名端口 / 特权端口（如 HTTP的80，HTTPS的443，SSH的22）。
    // 在 Linux 系统中，只有 root 用户（超级管理员）才有权限绑定 1024 以下的端口。
    if (port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }

    // 初始化 IPv4 地址结构体
    // `sin_family`: address 结构体的成员，表示地址族（Address Family）。
    // `AF_INET`: 明确告诉操作系统，这个地址结构体里装的是 IPv4 的地址和端口。
    addr.sin_family = AF_INET;  
    // `sin_addr`: 是一个内部结构体，用来存放 IP 地址。`s_addr` 是它里面实际存放 32位 IP 地址的整数变量。
    // `INADDR_ANY`: 这是一个宏，代表 `0.0.0.0`。表示将这个 Socket 绑定到这台机器上的所有可用网卡（IP）上
    // `htonl()`：`Host to Network Long` 的缩写。将小端序IP地址（电脑）转换为大端序（网络）
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    // `sin_port`: 结构体成员，用于存放 16 位的端口号。
    // `htons(port_)`：`Host to Network Short` 的缩写。把端口号也转换成网络大端序。
    addr.sin_port = htons(port_);

    // `struct linger`：Linux 提供的一个结构体，用来控制 `close()` 的行为。
    // `l_onoff = 1`：开关，`1` 代表开启 linger（逗留）特性。
    // `l_linger = 1`：逗留时间（秒）。表示如果服务器调用 `close()`
    // 但缓冲区里还有数据没发完，内核会让进程阻塞等待最多 1 秒钟。
    struct linger optLinger = { 0 };
    if (openLinger_) {
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }

    // `socket(...)`：这是向操作系统申请创建一个套接字的系统调用。
    // 参数 1 `AF_INET`：指定协议族为 IPv4。
    // 参数 2 `SOCK_STREAM`：声明要提供流式服务。流式服务代表面向连接、可靠、有序的数据流，这在底层直接对应了 TCP 协议。
    // 参数 3 `0`：表示使用默认协议。对于 `AF_INET` + `SOCK_STREAM` 的组合，默认协议就是 TCP。
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    // `setsockopt(...)`：设置套接字选项。
    // `SOL_SOCKET`：表示我们要设置的是“Socket 级别”的选项（相对于 TCP 级别或 IP 级别）。
    // `SO_LINGER`：明确指定要修改哪个选项（刚才提到的优雅关闭）。
    // *`&optLinger` (`optval`)：是一个指向包含了你想要设置的选项值（这里是一个 `struct linger` 类型的结构体）的内存地址指针。
    // 它告诉内核：“去这个地址拿我要设置的配置数据。”
    // `sizeof(optLinger)` (`optlen`)：是这个选项值数据的大小（字节数）。
    // 它告诉内核：“从刚才那个地址开始，准确读取这么多字节的数据。”
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if (ret < 0) {
        LOG_ERROR("Init linger error!", port_);
        close(listenFd_);
        return false;
    }

    int optval = 1;
    // `SO_REUSEADDR` 的作用：告诉内核，如果发现这个端口正在被 `TIME_WAIT` 状态的连接占用
    // 不要管它，允许我强行再次绑定并使用这个端口。
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    if (ret == -1) {
        LOG_ERROR("set socket setsockopt error!");
        close(listenFd_);
        return false;
    }

    // 绑定socket和它的地址
    // 把前面创建的的 socket (`listenfd`)，和具体的 IP 地址及端口号 (`address`) 绑定在一起。
    ret = bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前
    // 默认情况下，`socket()` 创建的套接字是主动的（用来发起连接，比如做客户端）。
    // `listen()` 函数的作用是把这个套接字变成被动的，告诉操作系统：
    // “这个 socket 是用来做服务器的，请开始监听有没有人向这个 IP 和端口发起连接”。
    ret = listen(listenFd_, 6);
    if (ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 把我们刚才配置好的 `listenFd_`交给 `epoll` 对象管理
    // EPOLLIN：注册可读事件。对于监听 socket 来说，所谓的“可读”，指的是有新的客户端发起连接
    // `listenEvent_`：类成员，通常会被设置为 `EPOLLET`（边缘触发模式）。
    ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);
    if (ret < 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }

    // `SetFdNonblock`：通过 `fcntl` 系统调用，将 `listenFd_` 设置为非阻塞（Non-blocking）。
    SetFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

// 利用 fcntl 将 Socket 设置为非阻塞。
int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    // `fcntl`用于控制文件描述符
    // `F_GETFL` (Get File Status Flags)：命令内核“把这个 fd 当前的状态标志全盘交给我”
    // 在保留原有属性的基础上，安全地“叠加”上 `O_NONBLOCK`。
    // `F_SETFL` (Set File Status Flags)：命令内核“把这个 fd 的状态标志替换为我传进来的 `new_option`”。
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}