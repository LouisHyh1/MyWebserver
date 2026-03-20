#include "httpconn.h"
using namespace std;

// 对 `HttpConn` 类的 `static`（静态）成员变量进行定义和分配内存。
const char* HttpConn::srcDir;
atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() {
    // 将文件描述符 `fd_` 置为 `-1`（无效值），地址 `addr_` 清零，并将状态 `isClose_` 标记为 `true`
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;
}

HttpConn::~HttpConn() {
    // 利用 C++ 的 RAII（资源获取即初始化）机制，在析构函数中调用 `Close()`
    Close();
}

// 初始化连接。
void HttpConn::init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    // 原子变量自增 1
    userCount++;  

    // 绑定当前的 Socket ID 和客户端 IP/端口。
    addr_ = addr;
    fd_ = fd;

    // 调用 `RetrieveAll`（通常是将读写指针复位到 0）可以瞬间清空缓冲区。
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();

    isClose_ = false;
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

// 主动关闭这个 HTTP 连接，释放绑定的系统 Socket 资源。
void HttpConn::Close() {
    // 解除映射（`munmap`），把内存还给操作系统
    response_.UnmapFile();
    if (!isClose_) {  // 防止重复关闭
        isClose_ = true;
        userCount--;
        close(fd_);  // 调用 Linux 系统函数关闭 Socket。
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

// 获取当前 HTTP 连接对应的文件描述符（File Descriptor）。
int HttpConn::GetFd() const { 
    return fd_; 
}
// 获取客户端的完整网络地址结构体。
struct sockaddr_in HttpConn::GetAddr() const { 
    return addr_; 
}
// 获取客户端的 IP 地址字符串。
const char* HttpConn::GetIP() const { 
    // `inet_ntoa` 是系统自带的函数，全称是 `Internet Network to ASCII`（网络字节转 ASCII 字符）。
    // 负责把那个晦涩的 32 位整数，翻译成人类能看懂的 `"xxx.xxx.xxx.xxx"` 格式的字符串
    return inet_ntoa(addr_.sin_addr);
}
// 获取客户端连接时使用的 端口号（Port）。
int HttpConn::GetPort() const {
    return addr_.sin_port;
}

// 处理 Socket 上的读事件。返回值 `ssize_t` 表示实际读写的字节数。
// 必须立即把当时的 `errno` 取出来保存到传入的指针里
ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1;
    // 如果 `isET == true`（边缘触发），Epoll 只会通知你这一次！
    // 如果你只调用一次 `ReadFd` 读了 1024 字节就走了，剩下的 976 字节将永远留在内核里
    // Epoll 不会再提醒你，这个连接就死锁了！
    // 在这个循环里，不断地去读内核，读到内核被彻底榨干
    // 这个时候 `ReadFd` 会把 `len` 置为 `<=0`，触发 `break` 跳出循环。
    do {
        len = readBuff_.ReadFd(fd_, saveErrno);
        if (len <= 0) break;
    } while (isET);
    return len;
}

// 处理 Socket 上的写事件。
ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1;
    do {
        // 1. 发起集中写调用
        // 将 `iov_[0]`（响应头）和 `iov_[1]`（静态文件）直接打包发给网卡。`len` 是这次**实际发送成功**的总字节数。
        len = writev(fd_, iov_, iovCnt_);

        // 2. 检查写入结果
        // 如果 `len <= 0`，说明 TCP 缓冲区满了（或者网络异常），立刻保存错误码并跳出循环等待下次 Epoll 可写事件。
        if (len <= 0) {
            *saveErrno = errno;
            break;
        }

        // 3. 检查是否全部发完
        // 如果两块 `iov` 的长度加起来等于 0，说明数据干干净净全发完了，完美收工，`break` 跳出。
        if (iov_[0].iov_len + iov_[1].iov_len == 0) {
            break;
        }
        // 情况 A：已发送的数据长度 > 响应头的长度
        else if (static_cast<size_t>(len) > iov_[0].iov_len) {
            // 说明响应头已经全部发完了，并且文件也发送了一部分！
            // 将文件块的指针向后移动（更新起始地址）
            // len - iov_[0].iov_len : 已发送文件块的长度
            // 强转成 `uint8_t*`（单字节指针）后，向后偏移已发送文件块的长度
            iov_[1].iov_base = static_cast<uint8_t*>(iov_[1].iov_base)
                             + (len - iov_[0].iov_len); 

            // 更新文件块剩余还需要发送的长度
            iov_[1].iov_len -= (len - iov_[0].iov_len);

            // 既然响应头全发完了，就清空它占用的信息
            if (iov_[0].iov_len != 0) {
                writeBuff_.RetrieveAll();  // 清空写缓冲区
                iov_[0].iov_len = 0;  // 响应头剩余待发长度置为 0
            }
        }
        // 情况 B：已发送的数据长度 <= 响应头的长度
        else {
            // 说明太惨了，连响应头都没发完

            // 把响应头的指针向后移动已发送的数据长度 
            iov_[0].iov_base = static_cast<uint8_t*>(iov_[0].iov_base) + len;

            // 更新响应头剩余待发长度
            iov_[0].iov_len -= len;

            // 更新写缓冲区的读取游标
            writeBuff_.Retrieve(len);
        }
    } while (isET || ToWriteBytes() > 10240);
    // 为什么还有一个 `ToWriteBytes() > 10240` (10KB)？这是一种启发式的性能调优：
    // 如果需要发的数据特别大（比如是个大视频）
    // 即使不是 ET 模式，我们也可以在单次可写事件中尝试多循环几次写入
    // 能够显著降低 Epoll 唤醒带来的上下文切换开销，提升大文件的下载速度。
    return len;
}

// 当 `read()` 将客户端的报文读取完毕后，调用此函数对报文进行解析，并生成准备发送的响应报文。
bool HttpConn::process() {
    request_.Init();

    // 如果读缓冲区里根本没有数据，直接返回 false
    if (readBuff_.ReadableBytes() <= 0) {
        return false;
    }
    // 1. 让 request_ 对象去解析读缓冲区里的 HTTP 文本
    else if (request_.parse(readBuff_)) {
        LOG_DEBUG("%s", request_.path().c_str());
        // 如果解析成功，准备给客户端返回 200 OK，并根据请求的路径 (request_.path()) 去寻找对应文件
        response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    }
    else {
        // 如果解析失败（比如客户端发来一堆乱码），准备返回 400 Bad Request
        response_.Init(srcDir, request_.path(), false, 400);
    }

    // 2. 根据刚刚 Init 好的信息，生成真正的 HTTP 响应报文头，存入写缓冲区 writeBuff_
    response_.MakeResponse(writeBuff_);

    // 3. 准备 iovec 的第一块：指向写缓冲区中“有效数据”的首地址
    // 也就是刚才生成的响应头的起始内存地址
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    // 4. 准备 iovec 的第二块：指向内存映射(mmap)出来的静态文件
    // 文件长度大于 0（有实际内容），且 `response_.File()` 不为空（说明通过 `mmap` 把磁盘上的文件成功映射到内存中了）。
    if (response_.FileLen() > 0 && response_.File() != nullptr) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;  // 把 `iovCnt_`（要发送的内存块数量）从 1 改为 2。
    }

    LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen(), iovCnt_, ToWriteBytes());
    return true;  // 告诉外界：一切准备就绪，可以触发写(Write)事件了！
}