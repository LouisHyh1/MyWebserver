#ifndef THREADCACHE_H
#define THREADCACHE_H
#include "Common.h"

class ThreadCache {
public:
	// 线程申请size大小的空间
	void* Allocate(size_t size);  

	// 回收线程中obj指向的大小为size的空间
	// obj:准备要归还给内存池的那块内存的起始地址
	void Deallocate(void* obj, size_t size);  

	// ThreadCache中空间不够时，向CentralCache申请空间的接口
	void* FetchFromCentralCache(size_t index, size_t alignSize);

	// tc向cc归还空间List桶中的空间
	void ListTooLong(FreeList& list, size_t size);
private:
	FreeList _freeLists[FREE_LIST_NUM];  // 自由链表数组（哈希桶），每个桶表示一个自由链表
};


// TLS：线程局部存储（TLS），是一种变量的存储方法，这个变量在它所在的线程内是全局可访问的，但是不能被其他线程访问到
// TLS的全局对象的指针，这样每个线程都能有一个独立的全局对象
static thread_local ThreadCache* pTLSThreadCache = nullptr;

#endif