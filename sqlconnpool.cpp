#include "sqlconnpool.h"
using namespace std;

// 构造函数，连接池对象刚被创建时，将“正在使用的连接数”和“空闲连接数”都初始化为 0。
SqlConnPool::SqlConnPool() {
    useCount_ = 0;
    freeCount_ = 0;
}

// 单例模式获取实例（懒汉式）
SqlConnPool* SqlConnPool::Instance() {
    // 局部静态变量在初始化时，是绝对线程安全的 
    // 即使有 100 个线程同时第一次调用 `Instance()`，编译器底层也会加锁，保证 `connPool` 只被创建一次。
    // 懒汉式实例在main()之后才创建
    static SqlConnPool connPool;
    // 返回这个唯一的实例的内存地址（指针）
    return &connPool;
}

// 初始化连接池，默认这个池子的大小是 10 个连接
void SqlConnPool::Init(const char* host, int port,
                       const char* user, const char* pwd,
                       const char* dbName, int connSize = 10) {
    assert(connSize > 0);
    // 准备创建 `connSize`（比如 10）个连接。
    for (int i = 0; i < connSize; i++) {
        // 每次循环开始，声明一个空的 MySQL 连接句柄指针 `sql`。
        MYSQL* sql = nullptr;
        // 调用 MySQL 的 C 语言官方 API：`mysql_init()`，用于初始化一个 MYSQL 结构体。
        sql = mysql_init(sql);
        if (sql == nullptr) {
            // 如果初始化失败（返回空指针）
            // 调用项目中自定义的日志宏 `LOG_ERROR` 打印报错日志，并用 `assert(sql)` 强行终止程序
            LOG_ERROR("MySql init error!");
            assert(sql != nullptr);
        }
        // 调用 API：`mysql_real_connect()`。
        // 这才是真正发起 TCP 网络请求、进行账号密码校验、连接到 MySQL 数据库的操作。
        sql = mysql_real_connect(sql, host, user, pwd, dbName, port, nullptr, 0);
        if (sql == nullptr) {
            LOG_ERROR("MySql Connect error!");
        }
        connQue_.emplace(sql);
    }
    // 记录最大连接数到 `MAX_CONN_`。
    MAX_CONN_ = connSize;
    // 调用 POSIX 信号量初始化函数 `sem_init()`。
    // `&semId_`：信号量对象的地址。
    // `0`：表示这个信号量只在当前进程的各个线程间共享。
    // `MAX_CONN_`：信号量的初始值被设定为 10（池子总容量）。
    sem_init(&semId_, 0, MAX_CONN_);
}

// 获取连接
MYSQL* SqlConnPool::GetConn() {
    // 声明一个空的 `sql` 指针，准备用来接收从队列里拿出来的连接。
    MYSQL* sql = nullptr;
    // 如果此刻池子空了，他不希望线程在这里死等，而是立刻打印一条警告日志 `"SqlConnPool busy!"`
    if (connQue_.empty()) {
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    // `sem_wait` 会让信号量减 1。
    // 如果此时信号量大于 0（比如还剩 3），它瞬间减到 2，线程畅通无阻，继续往下走。
    // 如果此时信号量刚好是 0，这个线程会被操作系统直接挂起（休眠）
    // 直到别的线程还回连接（执行了 `sem_post`）。
    sem_wait(&semId_);
    // {}:控制作用域
    {   
        // 声明一个锁卫士。一声明，立马死死锁住 `mtx_` 互斥锁。别人谁也碰不了队列。
        lock_guard<mutex> locker(mtx_);
        // 看一下队头的连接是谁，交给 `sql`。
        sql = connQue_.front();
        // 把队头这个连接从队列里踢出去（因为被借走了）。
        connQue_.pop();
    }
    // 大括号结束，`locker` 它的生命周期结束，自动解锁！
    return sql;
}

// 归还连接
void SqlConnPool::FreeConn(MYSQL* sql) {
    assert(sql != nullptr);
    lock_guard<mutex> locker(mtx_);
    // 把连接塞回队列的最尾巴（准备让下一个人用）。
    connQue_.emplace(sql);
    // 核心操作！信号量加 1！。这相当于拿着大喇叭喊：“池子里又有货啦！”
    // 如果有之前因为 `sem_wait` 而陷入沉睡的线程，操作系统会立刻把他一巴掌拍醒，让他去拿连接。
    sem_post(&semId_);
}

// 关闭连接池。在程序准备结束运行（退出）时调用。
void SqlConnPool::ClosePool() {
    lock_guard<mutex> locker(mtx_);
    while (!connQue_.empty()) {
        // 只要队列不是空的，就从队头拿出一个连接（`item`）。
        auto item = connQue_.front();
        connQue_.pop();
        // 调用 MySQL 的 C 语言 API：`mysql_close(item)`
        // 这会向 MySQL 服务器发送一个合法的断开 TCP 连接的请求，并释放本地客户端的内存资源。
        mysql_close(item);
    }
    // 调用 `mysql_library_end()`
    // 彻底清理 MySQL 整个 C API 库在使用过程中产生的全局缓存和资源，防止内存泄漏。关闭池子结束。
    mysql_library_end();
}

// 查询当前还剩多少个空闲连接。
int SqlConnPool::GetFreeConnCount() {
    // 因为获取队列的 `.size()` 在多线程下也存在读取脏数据的风险
    // 所以这里也非常严谨地使用了 `lock_guard` 先加锁，再返回大小。
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

// 析构函数
SqlConnPool::~SqlConnPool() {
    ClosePool();
}