#ifndef CENTRALCACHE_H
#define CENTRALCACHE_H

#include "Common.h"

class CentralCache {
public:
	// 单例接口（饿汉模式）
	static CentralCache* GetInstance() {
		return &_sInst;
	}

	// 从 cc 对应哈希桶的某个 Span 中，一口气截取一段（多个）连续的小块空间，打包交给 tc。
	// size_t batchNum： 这就是我们上一步算出来的 batchNum，代表 tc 期望拿到多少个块。
	// size_t size（规格单）： 这对应之前的 alignSize，代表每个块是多少字节（比如 8Byte、16Byte）。
	// 提出来的是多个用链表串起来的小块，所以 cc 需要把这串链表的头节点地址赋给 start，尾节点地址赋给 end。
	// 注意这里用的是指针的引用 (void*&)。这意味着函数内部可以直接修改外部传进来的指针指向。
	// 返回值：它返回的是 cc 实际给 tc 提供的块数。
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

	// 获取一个管理空间不为空的span
	Span* GetOneSpan(SpanList& list, size_t size);

	// 将tc还回来的多块空间放到span中
	void ReleaseListToSpans(void* start, size_t size);
private:
	// 构造函数私有化，并去掉拷贝构造和赋值
	CentralCache() {}  
	CentralCache(const CentralCache& copy) = delete;
	CentralCache& operator=(const CentralCache& copy) = delete;

	SpanList _spanLists[FREE_LIST_NUM];  // 以SpanList为元素的数组（哈希桶）
	static CentralCache _sInst;  // 饿汉模式创建一个CentralCache
};

#endif