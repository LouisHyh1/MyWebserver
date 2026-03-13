#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <assert.h>
#include <semaphore.h>
#include <thread>
#include "log.h"


class SqlConnPool {
public:
    // 单例模式的入口，返回一个指向这个类实例的全局唯一指针
    static SqlConnPool* Instance();

    // 获取连接
    MYSQL* GetConn();

    // 释放（归还）连接
    void FreeConn(MYSQL* conn);

    // 获取当前池子里还剩下多少个空闲可用的连接。
    int GetFreeConnCount();

    // 初始化连接池
    // `host`：数据库所在的 IP 地址。
    // `port`：数据库端口（MySQL 默认 3306）。
    // `user`：数据库登录用户名（如 `"root"`）。
    // `pwd`：数据库登录密码。
    // `dbName`：要连接的具体数据库库名。
    // `connSize`：指定这个池子一开始要创建多少个连接（即池子的容量）。
    void Init(const char* host, int port,
              const char* user, const char* pwd,
              const char* dbName, int connSize);
    
    // 关闭连接池。在程序准备结束运行（退出）时调用。
    // 它负责把池子里缓存的所有 `MYSQL` 连接真正的销毁，释放内存和网络资源，并清空队列。   
    void ClosePool();

private:
    // 构造函数 `SqlConnPool()` 和 析构函数 `~SqlConnPool()` 被硬性声明为了私有。
    SqlConnPool();
    ~SqlConnPool();
    
    // 池子的最大容量，也就是 `Init` 时传入的 `connSize`。
    int MAX_CONN_;
    // 当前已经被线程借走、正在使用中的连接数量。
    int useCount_;
    // 当前池子里还在排队、无人问津的空闲连接数量。
    int freeCount_;

    // 连接池的核心数据结构
    std::queue<MYSQL*> connQue_;
    // 保护队列的互斥锁
    std::mutex mtx_;
    // 控制连接数量的信号量
    sem_t semId_;
};

#endif