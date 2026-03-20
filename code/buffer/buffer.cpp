#include "buffer.h"


using std::string;

// 利用初始化列表初始化 
// `buffer_` 的大小为 `initBuffSize`（默认1024），读写游标都在起点（0）。
Buffer::Buffer(int initBuffSize) : buffer_(initBuffSize), 
readPos_(0), writePos_(0) {}

// 返回上述“三个区域”各自的长度。
// 完全对应前面画的【三个区域】的空间划分图。通过简单的减法计算出各个区域的字节数。
size_t Buffer::ReadableBytes() const {
	return writePos_ - readPos_;
}
size_t Buffer::WritableBytes() const {
	return buffer_.size() - writePos_;
}
size_t Buffer::PrependableBytes() const {
	return readPos_;
}

// 偷窥（获取）当前有效数据（Readable 区域）的首地址。
const char* Buffer::Peek() const {
	return BeginPtr_() + readPos_;
}

// 按长度归还
void Buffer::Retrieve(size_t len) {
	assert(len <= ReadableBytes());
	// 接把读游标向右移动 `len`。
	// 注意：这里并没有真正去清空物理内存的数据，只是移动了游标，逻辑上表示这块空间废弃了
	readPos_ += len;
}

// 按结束指针归还
void Buffer::RetrieveUntil(const char* end) {
	assert(Peek() <= end);
	Retrieve(end - Peek());
}

// 全部归还
void Buffer::RetrieveAll() {
	// `bzero` 是 POSIX 系统函数
	// 作用类似于 `memset(&buffer_[0], 0, buffer_.size())`，将底层内存全部置为零。
	bzero(&buffer_[0], buffer_.size());
	// 读写游标全部复位到 0
	readPos_ = 0;
	writePos_ = 0;
}

// 转为 string 并全部归还
string Buffer::RetrieveAllToStr() {
	string str(Peek(), ReadableBytes());
	RetrieveAll();
	return str;
}

// 获取当前可写区域（Writable）的首地址。
const char* Buffer::BeginWriteConst() const {
	return BeginPtr_() + writePos_;
}
char* Buffer::BeginWrite() {
	return BeginPtr_() + writePos_;
}

// 当外部向缓冲区写入了 len 长度数据后，调用此函数更新 writePos_ 游标。
void Buffer::HasWritten(size_t len) {
	// 向空闲区写完数据后，将 `writePos_` 向右移动 `len` 个字节。
	writePos_ += len;
}

// 这三个 `Append` 函数它们通过获取数据的首地址指针和长度
// 最终全部转调给下面那个核心的 `Append(const char* str, size_t len)`。
void Buffer::Append(const string& str) {
	Append(str.data(), str.size());
}
void Buffer::Append(const void* data, size_t len) {
	assert(data);
	Append(static_cast<const char*>(data), len);
}
void Buffer::Append(const Buffer& buff) {
	Append(buff.Peek(), buff.ReadableBytes());
}

void Buffer::Append(const char* str, size_t len) {
	assert(str);
	// 检查剩余空间够不够写 `len` 长度，不够就扩容或整理内存
	EnsureWriteable(len);
	// 将输入指针 `str` 到 `str+len` 范围内的数据，拷贝到以 `BeginWrite()` 为起点的内存中。
	std::copy(str, str + len, BeginWrite());
	// 写完后，移动写入游标。
	HasWritten(len);
}

// 确保缓冲区有 len 这么大的剩余空间可以写，不够就自动扩容。
void Buffer::EnsureWriteable(size_t len) {
	// 判断一下当前 `WritableBytes()` 
	// 如果小于需要写入的 `len`，就呼叫 `MakeSpace_` 来解决空间不足的问题。
	if (WritableBytes() < len) {
		MakeSpace_(len);
	}
	assert(WritableBytes() >= len);
}

// 从文件描述符 `fd`（通常是 socket）中读取数据到 Buffer 中。
// saveErrno：输出型参数，保存读取错误码。
ssize_t Buffer::ReadFd(int fd, int* saveErrno) {
	// 申请了一个 65535 字节（64KB）的栈上临时数组 `buff`。
	char buff[65535];

	// 定义了长度为 2 的 `iovec` 结构体数组。这是为 `readv`（分散读）做准备。
	// iovec为linux下自带的系统级结构体。
/*	struct iovec {
		void* iov_base;    // 缓冲区的起始地址 (Base address of a memory region)
		size_t iov_len;     // 这块缓冲区的长度 (Size of the memory region)
	};*/

	struct iovec iov[2];
	// 获取 Buffer 目前剩余的空闲可写空间大小 `writable`。
	const size_t writable = WritableBytes();

	// 第一块内存 (`iov[0]`)：直接指向 Buffer 内的 `Writable` 区域。能装多少装多少 (`writable`)。
	iov[0].iov_base = BeginPtr_() + writePos_;
	iov[0].iov_len = writable;
	// 第二块内存 (`iov[1]`)：指向栈上的 64KB 临时数组 `buff`。
	iov[1].iov_base = buff;
	iov[1].iov_len = sizeof(buff);

	// `readv`(Linux系统调用)：它从文件描述符 `fd` 中读取数据，首先会试图填满第一块内存 (`iov[0]`)，
	// 如果第一块内存满了，还有剩余数据，它会无缝继续填入第二块内存 (`iov[1]`)。
	// 这样只需一次系统调用，就能读取最多 `writable + 64KB` 的数据！
	const ssize_t len = readv(fd, iov, 2);

	// 如果 `len < 0`，说明读取出错了，保存当前系统的错误号 `errno` 到指针指向的变量中
	if (len < 0) {
		*saveErrno = errno;
	}
	// 如果 `len <= writable`：说明 Buffer 自己的空间足够大
	// 接收完了所有数据，连栈上的临时数组 `buff` 都没用到。
	// 此时只需正常向右移动写游标 `writePos_ += len`。
	else if (static_cast<size_t>(len) <= writable) {
		writePos_ += len;
	}
	// 说明数据太多，Buffer 自己的空间被撑爆了，多出来的数据被放到了栈上的 `buff` 里面。
	else {
		// 由于 Buffer 自己的空间已经被写满了，所以 `writePos_` 直接移动到 Buffer 最末尾
		writePos_ = buffer_.size();
		// 调用 `Append` 把栈上 `buff` 里多出的数据追加到 Buffer 中。多出的数据长度是 `len - writable`。
		// 这里调用 `Append` 会自动触发内部的 `EnsureWriteable`，从而让 vector 安全自动扩容！
		Append(buff, len - writable);
	}
	return len;
}

char* Buffer::BeginPtr_() {
	// *buffer_.begin()：解引用迭代器，得到首个元素的引用
	// &*buffer_.begin()：再取地址，从而拿到了底层字符数组的真正 C 风格裸指针
	// 在 C++11 中可以直接写 buffer_.data()
	return &*buffer_.begin();
}
const char* Buffer::BeginPtr_() const {
	return &*buffer_.begin();
}

void Buffer::MakeSpace_(size_t len) {
	// 如果 (剩余可写空间 + 头部已作废的空间) 加起来都不够你想写的长度 `len`，说明整个池子真装不下了。
	if (WritableBytes() + PrependableBytes() < len) {
		// 扩容后的大小为当前写位置加上 `len` 再加 1。
		buffer_.resize(writePos_ + len + 1);
	}
	// 有空间，但是碎片化了
	else {
		size_t readable = ReadableBytes();
		// `std::copy(原数据头, 原数据尾, 目标地址)`
		// 将中间的 Readable 真实有效数据，向左平移，覆盖掉前面的废弃空间，全部搬运到物理内存的最开头（索引0）。
		std::copy(BeginPtr_() + readPos_, BeginPtr_() + writePos_, BeginPtr_());
		// 搬家完成后，将 `readPos_` 复位到 0，并重新计算 `writePos_`。
		readPos_ = 0;
		writePos_ = readable;
		assert(readable == ReadableBytes());
	}
}