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

// `level`：一个整数，代表日志的严重级别
// `format`：一个字符指针，代表格式化字符串
// `...`：这是 C/C++ 的可变参数语法。意思是，除了前面两个参数，后面你想传多少个参数都可以
void Log::write(int level, const char* format, ...) {
    // `timeval` 是 Linux 系统里专门存时间的结构体，里面有两个核心变量：秒数（`tv_sec`）和微秒数（`tv_usec`）。
    struct timeval now{0, 0};
    // 调用 Linux 的系统函数获取当前时间。
    gettimeofday(&now, nullptr);
    // 定义一个专门存秒数的变量 `tSec`。
    time_t tSec = now.tv_sec;
    // 调用 `localtime` 函数，把干巴巴的“秒数”翻译成人类能看懂的年月日时分秒。
    struct tm* sysTime = localtime(&tSec);
    // 把指针指向的数据取出来，复制给 `t`。
    // 如果在多线程环境下，线程 A 刚拿到这个指针，还没来得及用，
    // 线程 B 也调用了 `localtime`，全局变量就被 B 覆盖了！A 再去读的时候，读到的就是 B 的时间。
    // 所以，必须立刻马上用 `t = *sysTime` 把数据深拷贝到自己私有的局部变量 `t` 里，防止被别的线程篡改。
    struct tm t = *sysTime;
    // 定义一个名为 `vaList` 的变量。
    // `va_list` 是 C 语言提供的一个专门用来处理 `...`（可变参数）的特殊指针。
    va_list vaList;

    // `toDay_ != t.tm_mday`：记录的日子和现实的日子对不上了！说明跨天（过零点）了。
    // `lineCount_ && (lineCount_ % MAX_LINES == 0)`：行数不是 0，并且行数除以最大行数余数为 0。
    // 说明当前文件写满了（比如正好写到了 50000 行）。
    if (toDay_ != t.tm_mday || (lineCount_ && (lineCount_ % MAX_LINES == 0))) {
        unique_lock<mutex> locker(mtx_);
        // 接下来的十几行代码，全都在做字符串拼接（计算新文件的名字）。
        // 字符串拼接在计算机里是非常耗费 CPU 时间的！
        // 而且这些计算用的全是 `t`、`tail` 等局部变量，不会和其他线程发生冲突。
        // 为了不让其他等待写日志的线程等太久，这里主动把锁解开
        locker.unlock();
        // `newFile`：用来存放最终生成的新文件绝对路径（比如 `/var/log/app/2023_10_25-1.log`）。
        char newFile[LOG_NAME_LEN];
        // `tail`：用来存放文件名中的“日期尾巴”（比如 `2023_10_25`）。长度给 36 足够了，并初始化为空。
        char tail[36] = {0};
        // 执行完后，`tail` 这个数组里就装了类似于 `"2023_10_25"` 这样的文字。
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        // 如果是因为跨天了才建的新文件，走这个分支。
        if (toDay_ != t.tm_mday) {
            // `path_`是目录（如 `./log`），`tail`是刚才算的日期，`suffix_`是后缀（如 `.log`）。
            // `LOG_NAME_LEN - 72` 是一种防御手段，告诉函数最多只能写这么多字，留出 72 个字节的安全缓冲，防止路径太长把内存挤爆。
            // `newFile` 变成了 `./log/2023_10_25.log`。
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);
            // 因为是跨天的新文件，所以把系统记录的“今天”更新为最新的日子；把行数重新清零。
            toDay_ = t.tm_mday;
            lineCount_ = 0;
        } 
        // 否则（说明没有跨天，是因为今天的文件行数写满了），走这个分支。
        else {
            // 拼接加上了分卷号的路径。
            // 怎么算分卷号的：`(lineCount_ / MAX_LINES)`，如果目前是 50000 行，除以 50000 就是 1。
            // 结果：`newFile` 变成了 `./log/2023_10_25-1.log`。
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s", path_, tail, (lineCount_ / MAX_LINES), suffix_);
        }
        // 文件名已经算完了，接下来我们要关闭旧文件，打开新文件！必须加锁！
        locker.lock();
        // 调用类内部的 `flush` 函数。在关掉旧文件之前，把内存缓冲区里还没来得及写到硬盘上的日志，强制刷进硬盘，防止丢失。
        flush();
        // 关闭当前的日志文件。`fp_` 是类成员变量，指向当前正在写的文件的指针。
        fclose(fp_);
        // 调用 C 标准库 `fopen`，打开我们刚刚算出来的 `newFile` 这个新文件。
        // 代表 Append（追加模式）。如果文件不存在就创建；如果存在，就在文件末尾继续写，不覆盖原来的。把新的文件指针重新赋值给 `fp_`。
        fp_ = fopen(newFile, "a");
        assert(fp_ != nullptr);
    } 

    {
        // 因为马上要向日志缓冲区里写具体的文字了，几十个线程如果同时写，文字会串行打架。必须加锁！
        unique_lock<mutex> locker(mtx_);
        // 我们要写一行新日志了，所以全局的总行数 +1。
        lineCount_++;

        // `buff_`：这是这个日志类自己的一个核心大仓库（内存缓冲区 Buffer）。
        // `buff_.BeginWrite()`：获取仓库中现在空余空间的起始内存地址。告诉 `snprintf`：“请把文字往这个地址里塞”。
        // `128`：安全限制，生成的时间前缀绝不可能超过 128 个字节。
        // `%06ld`：把 `now.tv_usec`（微秒）塞到这里，要求是 long 型整型（`ld`），并且必须凑够 6 位数，不够在前面补 0。
        // `n`：`snprintf` 会返回它实际上往仓库里成功写了多少个字符。把这个数字存到 `n` 里。
        int n = snprintf(buff_.BeginWrite(), 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);
        // 调用 `HasWritten(n)` 大声告诉buff_：“喂，我刚才往你里面写了 `n` 个字节，请把你的内部写指针往后挪 `n` 步！
        buff_.HasWritten(n);
        // 调用类内部的一个小助手函数。根据一开始传进来的 `level`（比如是 1）
        // 往仓库 `buff_` 里再追加几个字，比如 `"[INFO]: "`。
        AppendLogLevelTitle_(level);
        // 告诉vaList，“请从 `format` 这个参数的后面开始抓取数据”。
        va_start(vaList, format);
        
        // `vsnprintf`: 把用户想写的具体业务日志（比如 `"错误码: 404"`）写进仓库。
        // `vsnprintf`是 `snprintf` 的兄弟版本，专门用来吃 `vaList` 这种可变参数的。
        // `buff_.BeginWrite()`：再次获取仓库当前空余位置的起始地址（接在上一步的 `[INFO]: ` 后面）。
        // `buff_.WritableBytes()`：查询仓库现在还剩多少可用空间。防止用户写的日志太长，把仓库撑爆发生内存越界。
        // `format` 和 `vaList`：就是把用户的字符串格式和实际参数拼在一块。
        // 返回值 `m`：记录实际成功写入了多少个字节。
        int m = vsnprintf(buff_.BeginWrite(), buff_.WritableBytes(), format, vaList);
        // 规范操作。参数抓取完了，把 `vaList` 这个指针关闭清理掉，防止变成野指针引发内存问题。
        va_end(vaList);
        // 和前面一样，大声告诉 `buff_` 仓库：“我又往你里面写了 `m` 个字节，快把写指针再往后挪 `m` 步！
        buff_.HasWritten(m);
        // 在刚才写完的所有日志最后，强行加上两个特殊字符：`\n`（换行符）和 `\0`（C语言字符串结尾标志），长度为 2 个字节。
        // 没有 `\n`，所有的日志全都会挤在同一行，根本没法看。
        // 没有 `\0`，别的 C 语言函数去读这段内存时，就不知道字符串在哪里结束，会读出一堆乱码。
        buff_.Append("\n\0", 2);

        // 判断是否进行异步写入。
        // `isAsync_`：在初始化的时候，程序员有没有开启“异步模式”？
        // `deque_`：存放日志的“快递驿站”（阻塞队列）这个对象有没有被成功创建出来？
        // `!deque_->full()`：这个驿站现在没满吧？
        if (isAsync_ && deque_ && !deque_->full()) {
            // `buff_.RetrieveAllToStr()`：把 `buff_` 仓库里刚才拼好的一大串日志
            // 全部打包转换成一个 C++ 的 `std::string` 字符串对象。
            // `deque_->push_back(...)`：把这个包裹，直接扔进驿站（阻塞队列）的尾部。
            deque_->push_back(buff_.RetrieveAllToStr());
        } 
        // 比如根本没开异步,或者可怕的情况发生了：
        // 日志生成太快，快递驿站 `deque_` 已经被塞满了装不下了！
        else {
            // `buff_.Peek()`：获取缓冲区里刚才拼好的那段字符串的首地址。
            // `fp_`：我们当前打开的日志文件的指针。
            // `fputs`：C 语言系统函数，直接把字符串写入到文件里
            fputs(buff_.Peek(), fp_);
        }
        // 把 `buff_` 仓库的读写指针全部清零重置，假装它又变成了一个空仓库。
        // 这样当下一条日志过来调用 `Log::write` 时，又能从仓库的开头开始存放数据了。
        buff_.RetrieveAll();
    }
}

// 辅助函数：根据 level 添加日志级别前缀
void Log::AppendLogLevelTitle_(int level) {
    switch (level) {
    case 0:
        buff_.Append("[debug]: ", 9);
        break;
    case 1:
        buff_.Append("[info] : ", 9);
        break;
    case 2:
        buff_.Append("[warn] : ", 9);
        break;
    case 3:
        buff_.Append("[error]: ", 9);
        break;
    default:
        buff_.Append("[info] : ", 9);
        break;
    }
}


void Log::flush() {
    if (isAsync_) {
        deque_->flush();  // 如果是异步，唤醒队列（处理残余任务）
    }
    fflush(fp_);  // 强制将 C 标准库底层 FILE* 内部的缓冲立即写入操作系统的磁盘页缓存中
}

// 后台写日志线程的真正工作函数
void Log::AsyncWrite_() {
    string str{};
    // deque_->pop 是阻塞的。如果队列为空，后台线程会在这里休眠等待，不会消耗 CPU 资源
    // 只要成功取出了日志，就进入循环体
    while (deque_->pop(str)) {
        lock_guard<mutex> locker(mtx_);  // 操作文件指针 fp_ 必须加锁
        fputs(str.c_str(), fp_);  // 将取出的字符串写入文件
    }
}

// 单例模式获取实例（懒汉式）
Log* Log::Instance() {
    static Log inst;   // 局部静态变量。C++11 标准保证了其初始化的线程安全性。
    return &inst;  // 永远只返回这一个对象的地址
}

// 后台线程的入口函数
// `thread` 类需要一个静态函数或全局函数作为入口，不能直接传入类的非静态成员函数（因为缺少 `this` 指针）。
// 所以提供了一个静态方法 `FlushLogThread` 作为跳板，再去调用成员函数 `AsyncWrite_`。
void Log::FlushLogThread() {
    // 调用单例的 AsyncWrite_，死循环消费队列
    Log::Instance()->AsyncWrite_();
}