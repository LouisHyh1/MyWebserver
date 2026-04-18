#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <iostream>
#include <vector>
#include <atomic>
#include <assert.h>
#include <sys/uio.h>
#include <unistd.h>
#include "ConcurrentAlloc.h"



class Buffer {
public:
	Buffer(int initBuffSize = 1024);
	~Buffer() = default;  // 默认析构函数

	// 返回上述“三个区域”各自的长度。
	size_t WritableBytes() const;
	size_t ReadableBytes() const;
	size_t PrependableBytes() const;

	// 偷窥（获取）当前有效数据（Readable 区域）的首地址。
	const char* Peek() const;
	// 确保缓冲区有 len 这么大的剩余空间可以写，不够就自动扩容。
	void EnsureWriteable(size_t len);
	// 当外部向缓冲区写入了 len 长度数据后，调用此函数更新 writePos_ 游标。
	void HasWritten(size_t len);

	// 按长度归还
	void Retrieve(size_t len);
	// 按结束指针归还
	void RetrieveUntil(const char* end);
	// 全部归还
	void RetrieveAll();
	// 转为 string 并全部归还
	std::string RetrieveAllToStr();

	// 获取当前可写区域（Writable）的首地址。
	const char* BeginWriteConst() const;
	char* BeginWrite();

	// 重载了 4 个 Append 追加函数。
	// 方便使用者将 std::string、C 风格字符串、
	// 任意二进制数据（void*）甚至另一个 Buffer 对象里的数据拷贝到当前缓冲区的可写区域。
	void Append(const std::string& str);
	void Append(const char* str, size_t len);
	void Append(const void* data, size_t len);
	void Append(const Buffer& buff);

	// 从文件描述符 `fd`（通常是 socket）中读取数据到 Buffer 中。
	ssize_t ReadFd(int fd, int* Errno);
	// `WriteFd`：把 Buffer 里的有效数据写到 `fd` 中。
	ssize_t WriteFd(int fd, int* Errno);

	// ==========================================
    // 为 Buffer 专属重载 operator new 和 delete
    // ==========================================
    static void* operator new(size_t size) {
        // 调用你的定长内存池/并发内存池
        return ConcurrentAlloc(size); 
    }

    static void operator delete(void* ptr) {
        if (ptr == nullptr) return;
        ConcurrentFree(ptr);
    }
    
    // 如果有对象数组分配需求（虽然一般不会 delete[] Buffer），可以顺手重载
    static void* operator new[](size_t size) {
        return ConcurrentAlloc(size);
    }

    static void operator delete[](void* ptr) {
        if (ptr == nullptr) return;
        ConcurrentFree(ptr);
    }

private:
	// 获取底层 vector 数组最开始（索引 0）的内存地址。
	char* BeginPtr_();
	const char* BeginPtr_() const;
	// 如果空间不够了，要么进行内部内存搬移整理碎片，要么让 vector 扩容。
	void MakeSpace_(size_t len);

	// 底层的真身，用 C++ 标准库动态数组管理内存，防止内存泄漏。
	std::vector<char> buffer_;
	// 起始位置游标
	std::atomic<size_t> readPos_;
	std::atomic<size_t> writePos_;
};

#endif