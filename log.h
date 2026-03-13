#ifndef LOG_H
#define LOG_H

#include <mutex>              // C++11互斥锁，用于线程同步，保护共享数据
#include <string>             // 标准字符串类 std::string
#include <thread>             // C++11多线程库 std::thread，用于创建后台写日志线程
#include <sys/time.h>         // Linux系统调用，获取微秒级高精度时间 (gettimeofday)
#include <string.h>           // C风格字符串处理函数 (strcpy, memset等)
#include <stdarg.h>           // C标准库，处理可变参数列表 (va_list, va_start, va_end)
#include <assert.h>           // 引入 assert 断言，用于调试阶段捕获逻辑错误
#include <sys/stat.h>         // Linux系统调用，文件状态信息，这里主要为了使用 mkdir 创建目录
#include "blockqueue.h"       // 自定义：线程安全的阻塞队列（异步日志的核心容器）
#include "buffer.h" // 自定义：可自动扩容的缓冲区（用于拼接和格式化单条日志）

class Log {
public:
    // 初始化日志系统
    // `level`：日志级别（决定哪些日志需要被打印）。
    // `path` & `suffix`：日志文件存放的目录（默认 ./log）和后缀名（默认 .log）。
    // `maxQueueCapacity`（核心设计）：阻塞队列的最大容量。
    // 如果传入 0，表示开启“同步日志”（直接写磁盘）；
    // 如果传入 >0 的数，表示开启“异步日志”（放入队列，由后台线程写磁盘）。
    void init(int level, const char* path = "./log",
              const char* suffix = "/log", 
              int maxQueueCapacity = 1024);
    
    // 单例模式获取实例          
    static Log* Instance();
    
    // 异步日志的工作线程函数
    // 必须是 static 的。
    // 因为 `std::thread` 在绑定类成员函数时，普通成员函数隐含了 `this` 指针
    // 而静态成员函数没有 `this` 指针，可以直接作为线程的回调函数。
    static void FlushLogThread();
    
    // 写入日志的核心函数
    void write(int level, const char* format, ...);
    
    // 强制刷新缓冲区到磁盘
    void flush();
    
    // 状态获取与设置
    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() { return isOpen_; }

private:
    Log();
    // `virtual` 析构函数是为了防止潜在的继承导致内存泄漏
    virtual ~Log();
    
    // 用于在日志前面加上 `[INFO]`, `[ERROR]` 这样的标签。
    void AppendLogLevelTitle_(int level);

    // 这就是后台写线程真正执行的死循环函数。它会不断从 `deque_` 取出字符串，然后写进 `fp_` (文件)。
    void AsyncWrite_();

private:
    // 静态常量，属于整个类，节省内存
    static const int LOG_PATH_LEN = 256; // 路径最大长度
    static const int LOG_NAME_LEN = 256; // 文件名最大长度
    static const int MAX_LINES = 50000;  // 单个文件最大日志行数

    const char* path_;     // 日志路径指针
    const char* suffix_;   // 日志后缀名指针
    int MAX_LINES_;        // 实际限制的最大行数
    int lineCount_;        // 当前日志文件已经写入的行数（用于判断是否需要创建新文件）
    int toDay_;            // 记录今天是哪一天（用于按天轮转日志文件）
    bool isOpen_;          // 日志系统是否正常开启
 
    Buffer buff_;          // 用户层面的输出缓冲区。每次写日志先写进这里，再压入队列或写磁盘
    int level_;            // 当前全局的日志输出等级（例如：设置为WARN，则INFO和DEBUG统统不输出）
    bool isAsync_;         // 标识当前是异步模式还是同步模式

    FILE* fp_;             // C标准库的文件指针。相比 C++ 的 std::fstream，C 的 fwrite 性能更高，常用于底层。
    
    // 智能指针管理的动态对象
    std::unique_ptr<BlockDeque<std::string>> deque_; 
    std::unique_ptr<std::thread> writeThread_;
    
    std::mutex mtx_;       // 互斥锁，保证多线程写日志时 lineCount_, buff_ 等成员变量的安全
};

// IsOpen()： 检查日志系统是否处于正常工作状态。
// 如果后台文件打开失败、磁盘满了或者程序正在优雅退出，这个开关会被关闭，直接丢弃后续操作

// GetLevel() <= level（日志级别过滤）
// 根据底下的定义，级别映射为：DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3。
// 让开发者可以在开发环境下打印所有细节（设为 0）
// 而在生产环境下只打印关键和错误信息（设为 1 或 2），从而极大节省生产环境的 CPU 和磁盘 I/O 开销。

// 一旦通过了前面的拦截器，核心的 write 函数就被调用了。
// 它接收日志级别、格式化字符串（如 "%d errors"）和具体的变量。

// log->flush():强制落盘/唤醒消费者
// 如果是同步日志，它会强制操作系统立刻把内存里的日志刷新到物理磁盘上。
// 如果是异步日志，这里的 flush() 里面很可能就包含了 condConsumer_.notify_one()，用来立刻唤醒后台正在休眠的消费者线程
#define LOG_BASE(level, format, ...) \
    do {\
        Log* log = Log::Instance();\
        if (log->IsOpen() && log->GetLevel() <= level) {\
            log->write(level, format, ##__VA_ARGS__);\
            log->flush();\
        }\
    } while (0)\

// 向用户提供傻瓜式的 API。用户不需要知道什么是 `LOG_BASE`，不需要传等级。直接用：
#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while (0)
#define LOG_INFO(format, ...) do {LOG_BASE(1, format, ##__VA_ARGS__)} while(0)
#define LOG_WARN(format, ...) do {LOG_BASE(2, format, ##__VA_ARGS__)} while(0)
#define LOG_ERROR(format, ...) do {LOG_BASE(3, format, ##__VA_ARGS__)} while(0)

#endif