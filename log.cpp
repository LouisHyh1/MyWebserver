#include "log.h"
using namespace std;

Log::Log() {
    lineCount_ = 0;       // 当前日志文件的行数初始化为0
    isAsync_ = false;     // 默认不开启异步模式，使用同步写入
    writeThread_ = nullptr; // 后台写日志的线程指针初始化为空
    deque_ = nullptr;     // 阻塞队列（用于异步）指针初始化为空
    toDay_ = 0;           // 记录当前日志文件对应的日期（几号），初始化为0
    fp_ = nullptr;        // 文件指针初始化为空
}

Log::~Log() {
    // 1. 如果写线程存在，并且该线程还可以被 join（正在运行中）
    if(writeThread_ && writeThread_->joinable()) {
        // 只要阻塞队列里还有日志没写完
        while(!deque_->empty()) {
            deque_->flush(); // 唤醒消费者线程，让它赶紧把剩下的日志消费掉
        };
        deque_->Close();     // 关闭阻塞队列，禁止再写入新日志
        writeThread_->join(); // 阻塞等待后台写线程执行完毕，安全退出
    }
    // 2. 如果文件指针处于打开状态
    if(fp_) {
        // 加锁，防止此时还有其他线程尝试写入
        lock_guard<mutex> locker(mtx_); 

        // 强制把 C 标准库缓冲区的内容刷入磁盘
        // 为了防止日志数据丢失，确保在关闭文件之前
        // 所有驻留在内存中的日志数据都被完整地写入到操作系统或磁盘中。
        flush();         

        // 关闭文件，释放系统资源               
        fclose(fp_);                    
    }
}

int Log::GetLevel() {
    lock_guard<mutex> locker(mtx_); // 加锁保护
    return level_;                  // 返回当前日志过滤级别
}

void Log::SetLevel(int level) {
    lock_guard<mutex> locker(mtx_); // 加锁保护
    level_ = level;                 // 设置新的日志级别
}

void Log::init(int level = 1, const char* path, const char* suffix, int maxQueueSize) {
    isOpen_ = true;  // 标记日志系统开启
    level_ = level;  // 设置日志级别

    // 决定是否开启异步模式的核心逻辑
    if (maxQueueSize > 0) {
        isAsync_ = true;  // 队列最大长度大于0，说明需要开启异步
        if (deque_ == nullptr) {  // 如果队列还没被创建过
            // 创建一个存放 string 的阻塞队列
            auto newDeque = make_unique<BlockDeque<string>>();
            // 使用 move 转移所有权给成员变量 deque_
            deque_ = move(newDeque);

            // 创建一个后台专门写日志的线程，线程函数是 FlushLogThread
            auto NewThread = make_unique<thread>(FlushLogThread);
            // 转移所有权给 writeThread_
            writeThread_ = move(NewThread); 
        }
        else {
            isAsync_ = false;  // 队列大小为0，使用同步写入
        }
    }
    lineCount_ = 0;  // 初始化日志行数为0

    // 获取当前时间戳
    time_t timer = time(nullptr);

    // 把秒数翻译成人类看懂的年月日
    // 把timer转换成一个名叫 `tm` 的结构体指针。
    //  `tm` 结构体里面包含了年、月、日、时、分、秒等单独的变量。
    struct tm* sysTime = localtime(&timer);

    // 把指针指向的那个结构体里的数据完完整整地拷贝一份
    // 为什么不直接用指针？
    // 因为 `localtime()` 返回的是 C 标准库内部的一个全局共享内存块的指针。
    // 如果不赶紧拷贝出来，万一程序里其他地方也调了时间函数，这块内存的数据就被覆盖篡改了。
    // 拷贝出来是最安全的做法。
    struct tm t = *sysTime;

    path_ = path;  // 保存日志保存路径，比如 "./log"
    suffix_ = suffix;  // 保存日志文件后缀，比如 ".log"

    // 在内存里申请一段连续的空间（字符数组），长度是 `LOG_NAME_LEN`
    char fileName[LOG_NAME_LEN] = {0};

    // `snprintf` 是 C/C++ 里极其常用的安全字符串拼接函数。
    // 参数 2 `LOG_NAME_LEN - 1`：最多只能写 255 个字符，留 1 个位置给字符串结尾符号 `\0`，
    // 绝对防止内存溢出（极其严谨的写法）。
    // 参数 3 `"%s/%04d_%02d_%02d%s"`：这是模版（占位符）。
    // `t.tm_year + 1900`：为什么加 1900？因为 C 语言规定，`tm_year` 存的是从 1900 年起过了多少年。
    // `t.tm_mon + 1`：为什么加 1？因为 C 语言规定，月份是从 0 开始算的
    // 这句代码执行完，`fileName` 里面存的字符串就会变成类似：`"./log/2023_10_25.log"`。
    snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s",
             path_, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, suffix_);
    // 记录当前是几号，用于后续判断是否需要按天滚动日志
    toDay_ = t.tm_mday;
    {
        lock_guard<mutex> locker(mtx_);  // 锁定互斥量，开始操作文件
        buff_.RetrieveAll();  // 清空内存缓冲区 buff_ 中的遗留数据

        if (fp_ != nullptr) {  // 如果之前已经打开了旧文件
            flush();  // 刷盘
            fclose(fp_);  // 关闭旧文件
        }

        fp_ = fopen(fileName, "a");  // 以追加模式("a")打开新日志文件
        if (fp_ == nullptr) {  // 如果打开失败（通常是因为目录不存在）
            mkdir(path_, 0777);  // 创建目录，权限设置为可读可写可执行
            fp_ = fopen(fileName, "a");  // 再次尝试打开文件
        }
        assert(fp_ != nullptr);  // 断言文件必须打开成功，否则程序直接崩溃退出
    }
}