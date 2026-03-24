#pragma once
#include <iostream>
#include <vector>
#include <thread>
#include <assert.h>
#include <mutex>
#include <cstring>
#include <atomic>
#include <unordered_map>
#define NOMINMAX  // 告诉 Windows 库别搞这两个宏（min和max）
#ifdef _WIN64
#include <Windows.h>  // Windows下的头文件
#else
// Linux相关头文件
#include <sys/mman.h> // 提供 mmap 和 munmap
#include <unistd.h>   // 提供 brk、sbrk（辅助备用）
#endif


using std::cout;
using std::endl;
using std::vector;


static const size_t FREE_LIST_NUM = 208;  // 哈希桶中的自由链表个数
static const size_t MAX_BYTES = 256 * 1024;  // ThreadCache单次申请的最大字节数
static const size_t PAGE_NUM = 129;  // span的最大管理页数

#ifdef _WIN64
static const size_t PAGE_SHIFT = 13;  // 一页多少位，这里给一页8KB，也就是13位
#else
static const size_t PAGE_SHIFT = 12;  // 一页多少位，linux下一页4KB，也就是12位
#endif

typedef size_t PageID;

inline static void*& ObjNext(void* obj) {
	return *reinterpret_cast<void**>(obj);
}

// 直接去堆上按页申请空间，kpage为申请页的数量
inline static void* SystemAlloc(size_t kpage) {
#ifdef _WIN64 
	// VirtualAlloc：Windows下的系统调用接口
	void* ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	// mmap 系统调用: 其核心作用是向操作系统直接申请分配一块匿名的、可读写的虚拟内存。
	// void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
	// nullptr (对应 addr) - 期望起始地址, 传入 nullptr表示不强制指定地址，全权交由操作系统自行决定
	// kpage << PAGE_SHIFT (对应 length) - 分配的内存长度（字节数）
	// PROT_READ | PROT_WRITE: 可读可写
	// MAP_PRIVATE | MAP_ANONYMOUS: 建立匿名私有映射（分配纯粹的物理内存）
	// -1 (对应 fd) - 文件描述符，在 POSIX 标准中，进行匿名映射时，这个参数通常需要被设置为 -1 以保证跨平台的兼容性。
	// 0 (对应 offset) - 文件偏移量，因为是匿名映射，没有文件，所以偏移量毫无意义，严格规定必须传入 0。
	// 返回值：void* ptr，返回分配好的内存的起始地址，并赋值给指针 ptr。
	void* ptr = mmap(nullptr, kpage << PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	// mmap 失败时返回 MAP_FAILED (即 (void*)-1 )，而不是 nullptr
	if (ptr == MAP_FAILED) {
		ptr = nullptr;
	}
#endif

	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}

// 直接去堆上释放空间
// 注意：Linux 的 munmap 强制要求传入释放空间的大小！
// 为了兼容原有的设计，增加了 kpage 参数（默认值为0，防止Windows代码报错），
// 但在实现 PageCache 回收 span 时，在 Linux 环境下调用此函数务必传入该 span 管理的页数 (span->_n)！
inline static void SystemFree(void* ptr, size_t kpage = 0) {
#ifdef _WIN64
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// Linux下使用 munmap 释放空间
	munmap(ptr, kpage << PAGE_SHIFT);
#endif
}

class FreeList {
public:
	// 判断哈希桶是否为空
	bool Empty() {  
		return _freeList == nullptr;
	}
	// 用于回收空间
	void Push(void* obj) {  
		// 头插
		assert(obj != nullptr);  // 插入非空空间

		ObjNext(obj) = _freeList;
		_freeList = obj;

		++_size;  // 插入一块，size + 1
	}
	// 用于提供空间
	void* Pop() {  
		// 头删
		assert(_freeList != nullptr);  // 提供空间的前提要有空间

		void* obj = _freeList;
		_freeList = ObjNext(obj);

		--_size;  // 去掉一块，_size - 1

		return obj;
	} 
	// 向自由链表进行头插，且插入多块空间
	// size表示当前这个范围内的块数
	void PushRange(void* start, void* end, size_t size) {
		ObjNext(end) = _freeList;
		_freeList = start;

		_size += size;
	}
	// FreeList未达上限时，能够申请的最大块空间是多少
	size_t& MaxSize() {
		return _maxSize;
	}
	// 获取当前桶中有多少块空间
	size_t Size() {
		return _size;
	}
	// 删除掉桶中n个块（头删），并把删除的空间作为输出型参数返回
	void PopRange(void*& start, void*& end, size_t n) {
		// 删除块数不能超过_size块
		assert(n <= _size);
		
		start = end = _freeList;
		// 让end走n-1步
		for (size_t i = 0; i < n - 1; ++i) {
			end = ObjNext(end);
		}
		// 将链表头指针指向end的下一块
		_freeList = ObjNext(end);  
		// 再把end的下一块置空
		ObjNext(end) = nullptr;
		// 减去删除的块数
		_size -= n;  
	}
private:
	void* _freeList = nullptr;  // 自由链表头指针，初始为空
	// 当前自由链表申请未达到上限时，能够申请的最大块空间是多少
	// 初始值给1，表示第一次只能申请一块
	// 到了上限之后_maxSize这个值就不再继续增加了
	size_t _maxSize = 1;
	size_t _size = 0;  // 当前自由链表中有多少块空间
};

class SizeClass {
public:
	// 计算对齐字节数
	// size 代表的是单个内存块的字节数
	static size_t RoundUp(size_t size) {
		if (size <= 128) {  // [1, 128] 8B
			return _RoundUp(size, 8);
		}
		else if (size <= 1024) {  // [129, 1024] 16B
			return _RoundUp(size, 16);
		}
		else if (size <= 8 * 1024) {  // [1025, 8*1024] 128B
			return _RoundUp(size, 128);
		}
		else if (size <= 64 * 1024) {  // [8*1024+1, 64*1024] 1024B
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024) {  // [64*1024+1, 256*1024] 8*1024B
			return _RoundUp(size, 8 * 1024);
		}
		else {
			// 单词申请空间大于256KB，直接按照页来对齐
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}
	// 辅助函数：计算每个分区对应的对齐后的字节数(alignNum是size对应分区的对齐数)
	static size_t _RoundUp(size_t size, size_t alignNum) {
		// 经典位运算：向上取整并对齐到 2 的次幂
		return (size + alignNum - 1) & ~(alignNum - 1);
	}
	// 计算映射的自由链表桶下标
	static inline size_t Index(size_t size) {
		assert(size <= MAX_BYTES);
		// 每个区间有多少个自由链表
		// 16: [1, 128] 区间按 8 对齐，一共需要 128 / 8 = 16 个桶。
		// 56: [129, 1024] 区间按 16 对齐，一共有 1024 - 128 = 896 字节，需要 896 / 16 = 56 个桶。后同
		static int group_array[4] = { 16, 56, 56, 56 };
		if (size <= 128) {
			// [1,128] 8B -->8B就是2^3B，对应二进制位为3位
			return _Index(size, 3); // 3是指对齐数的二进制位位数，这里8B就是2^3B，所以就是3
		}
		else if (size <= 1024) {
			// [128+1,1024] 16B -->4位
			// 假设size = 130（属于第二个区间，按 16 字节对齐）
			// size - 128：把前面的 128 字节扣除。我们只算它相对于第二个区间起点的偏移量，即 130 - 128 = 2 字节。
			// _Index(2, 4)：计算这多出来的 2 字节在第二个区间的相对下标。
			// 通过上面公式 ((2 + 16 - 1) >> 4) - 1 = 0。说明它是第二个区间的第 0 个桶。
			// + group_array[0]：相对下标算出来了，还要加上前面所有区间已经用掉的桶的数量。
			// 前面第一个区间用掉了 16 个桶（下标 0~15），所以它的全局绝对下标应该是 0 + 16 = 16。
			return group_array[0] + _Index(size - 128, 4);
		}
		else if (size <= 8 * 1024) {
			// [1024+1,8*1024] 128B -->7位
			return group_array[0] + group_array[1] + _Index(size - 1024, 7);
		}
		else if (size <= 64 * 1024) {
			// [8*1024+1,64*1024] 1024B -->10位
			return group_array[0] + group_array[1]
				+ group_array[2] + _Index(size - 8 * 1024, 10);
		}
		else if (size <= 256 * 1024) {
			// [64*1024+1,256*1024] 8 * 1024B  -->13位
			return group_array[0] + group_array[1]
				+ group_array[2] + group_array[3] + _Index(size - 64 * 1024, 13);
		}
		else {  // 超过了256KB，直接报错
			assert(false);
			return -1;
		}

	}
	// 辅助函数：求size对应的自由链表在哈希桶中的下标（align_shift: 对齐数的基数（2的几次幂））
	static inline size_t _Index(size_t size, size_t align_shift) {
		// 以 size = 3，按 8 字节对齐 align_shift = 3 为例
		// 1 << align_shift：左移操作，等价于 2^3 = 8。这就是还原出对齐数 8。
		// size + (1 << align_shift) - 1：即 3 + 8 - 1 = 10
		// 为什么要加上 (对齐数 - 1)？ 这是一个在 C/C++ 中非常经典的向上取整技巧。
		// 如果申请 1~8 字节，加上 7 之后，范围就变成了 8~15；如果申请 9~16 字节，加上 7 就变成了 16~23。
		// >> align_shift：右移操作，等价于除以对齐数。即 10 >> 3 等价于 10 / 8 = 1。
		// 到这一步，1~8 字节除以 8 都会得到 1；9~16除以 8 都会得到 2。
		// 最后 - 1：因为数组的下标是从 0 开始的。
		// 1是int型（32位），运算时可能溢出，所以使用static_cast<size_t>转换为size_t类型
		return ((size + (static_cast<size_t>(1) << align_shift) - 1) >> align_shift) - 1;
	}
	// 计算单次申请上限
	static size_t NumMoveSize(size_t size) {
		assert(size > 0);  // 不能申请0大小的空间

		// MAX_BYTES就是单个块的最大空间，也就是256KB
		size_t num = MAX_BYTES / size;

		// 比如说单次申请的是8B，256KB除以8B得到的是一个三万多的数
		// 那这样单次上限三万多块太多了，直接开到三万多可能会造
		// 成很多浪费的空间，不太现实，所以该小一点
		if (num > 512) num = 512;
			
		// 比如说单次申请的是256KB，那除得1，如果256KB上限一直是1，
		// 那这样有点太少了，可能线程要的是4个256KB，那将num改成2
		// 就可以少调用几次，也就会少几次开销，但是也不能太多
		// 256KB空间是很大的，num太高了不太现实，可能会出现浪费
		if (num < 2) num = 2;
			
		//[2，512]，一次批量移动多少个对象的（慢启动）上限值
		//小对象一次批量上限高
		//大对象一次批量上限低

		return num;
	}
	// 块页匹配算法
	// 核心目标：当 CentralCache 里 size 大小的小块内存用完时
	// 为了既不浪费内存，又不用频繁向底层申请
	// 到底该向 PageCache 批发“多少页”的内存（Span）最合适？
	static size_t NumMovePage(size_t size) {  // size表示一块的大小
		// 当cc中没有span为tc提供小块空间时，cc就需要向pc申请一块span，此时需要根据一块空间的大小来匹配
		// 出一个维护页空间较为合适的span，以保证span为size后尽量不浪费或不足够还再频繁申请相同大小的span

		// NumMoveSize是算出tc向cc申请size大小的块时的单次最大申请块数
		size_t num = NumMoveSize(size);
		// num * size就是单次申请最大空间大小
		size_t npage = num * size;
		// PAGE_SHIFT表示一页要占用多少位，比如一页8KB就是13位
		// 这里右移其实就是除以页大小，算出来就是单次申请最大空间有多少页
		npage >>= PAGE_SHIFT;
		// 如果算出来为0，那就直接给1页，比如说size为8B时，num就是512
		// npage算出来就是4KB，那如果一页8KB，算出来直接为0了，意思就是半页的空间都
		// 够8B的单次申请的最大空间了，但是二进制中没有0.5，所以只能给1页
		if (npage == 0) npage = 1;
		return npage;
	}
};

// 以页为基本单位的结构体（一段连续的页空间）
struct Span {
public:
	PageID _pageID = 0;  // 页号
	size_t _n = 0;  // 当前span管理的页的数量
	size_t _objSize = 0;  // span管理页被切分成的块有多大

	void* _freeList = nullptr;  // 每个span下面挂的小块空间的头指针
	size_t use_count = 0;  // 当前span分配出去了多少个块空间

	Span* _prev = nullptr;  // 前一个结点
	Span* _next = nullptr;  // 后一个结点

	bool _isUse = false;  // 判断当前span在cc中还是在pc中
};

class SpanList {
public:
	SpanList() {
		// 构造哨兵头结点
		_head = new Span();

		// 因为是双向循环，所以都指向_head
		_head->_next = _head;
		_head->_prev = _head;
	}
	// 将新的 Span 节点（ptr）插入到指定节点（pos）的前面。
	void Insert(Span* pos, Span* ptr) {
		// 二者都不能为空
		assert(pos != nullptr);
		assert(ptr != nullptr);

		Span* prev = pos->_prev;  // 找到 pos 的前驱节点

		// 步骤 1: 将 prev 和新节点 ptr 连接起来
		prev->_next = ptr;
		ptr->_prev = prev;

		// 步骤 2: 将新节点 ptr 和 pos 连接起来
		ptr->_next = pos;
		pos->_prev = ptr;
	}
	// 删除pos指向的结点
	void Erase(Span* pos) {
		assert(pos != nullptr);
		assert(pos != _head);  // 绝对不能把作为哨兵的头结点删掉

		Span* prev = pos->_prev;  // 找到 pos 的前驱节点
		Span* next = pos->_next;  // 找到 pos 的后继节点

		// 将 prev 和 next 直接连接，从而架空 pos
		prev->_next = next;
		next->_prev = prev;
	}
	// 头插
	void PushFront(Span* span) {
		Insert(Begin(), span);
	}
	// 删除掉第一个span
	Span* PopFront() {
		// 先获取_head后面的第一个span
		Span* front = _head->_next;
		// 删除掉这个span，直接复用Erase
		Erase(front);
		//返回原来的第一个span
		return front;
	}
	// 判空（判断spanlist中是否有span）
	bool Empty() {
		// 带头结点双向循环链表空的时候_head->next指向自己
		return _head == _head->_next;
	}
	// 头结点
	// _head为虚拟头结点，因此第一个有效数据为_head->_next
	Span* Begin() {
		return _head->_next;
	}
	// 尾结点
	// End()指向最后一个有效元素的下一个位置，即虚拟头结点_head（双向循环链表）
	Span* End() {
		return _head;
	}
private:
	Span* _head;  // 哨兵头结点（虚拟头结点）
public:
	std::mutex _mtx;  // 每个CentralCache中的哈希桶都要有一个桶锁
};