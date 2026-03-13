#ifndef SQLCONNRAII_H
#define SQLCONNRAII_H
#include "sqlconnpool.h"

// 资源在对象构造初始化 资源在对象析构时释放，用这个类来实现
class SqlConnRAII {
public:
    // 构造函数，第一个参数为传入的连接
    // 由于连接本身为MYSQL*类型，所以要传它的引用才能对他进行修改
    // connpool：告诉这个类从哪个池子（`connpool`）去拿连接。
    SqlConnRAII(MYSQL*& sql, SqlConnPool* connpool) {
        // 池子必须真实存在
        assert(connpool != nullptr);
        // 向连接池发起请求，通过互斥锁和信号量，安全地拿到了一个 `MYSQL*`
        sql = connpool->GetConn();
        sql_ = sql;
        connpool_ = connpool;
    }
    // 析构函数
    // 只要这个 `SqlConnRAII` 对象被销毁（不管是因为函数正常结束 `}`
    // 还是因为 `return`，甚至是因为抛出了 `throw Exception`，C++ 语言机制绝对保证会执行这里！
    ~SqlConnRAII() {
        // 先检查一下当时是不是真的借到了连接
        if (sql_ != nullptr) {
            // 如果借到了，调用池子的释放方法，把 `sql_` 还回去
            connpool_->FreeConn(sql_);
        }
    }
private:
    MYSQL* sql_;  // 保存当前负责管理的那个数据库连接的指针。
    SqlConnPool* connpool_;  // 保存这个连接所属的池子的指针。
};

#endif