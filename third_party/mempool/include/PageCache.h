#pragma once
#include "Common.h"
#include "ObjectPool.h"
#include "PageMap.h"

class PageCache {
public:
	std::mutex _pageMtx;  // pc整体的锁
private:
	SpanList _spanLists[PAGE_NUM];  // pc中的哈希桶，每个桶都是一个SpanList
	static PageCache _sInst;  // 单例类对象

	// 哈希映射，用来快速通过页号找到对应span
	// std::unordered_map<PageID, Span*> _idSpanMap;
	// WIN64下，64 位系统的用户态虚拟地址空间最多只用到 48 位
	// Linux下，用户态虚拟地址上限飙升到了 57 位
	#ifdef _WIN64
	TCMalloc_PageMap3<48 - PAGE_SHIFT> _idSpanMap;
	#else 
	// 因为 Linux 指针范围是 47 位，所以 BITS 必须至少为 47！
	TCMalloc_PageMap3<57 - PAGE_SHIFT> _idSpanMap;
	#endif
	
	ObjectPool<Span> _spanPool;  // 创建span的对象池
public:
	// 饿汉单例
	static PageCache* GetInstance() {
		return &_sInst;
	}
	// pc从_spanLists中拿出来一个k页的span
	Span* NewSpan(size_t k);
	// 通过页地址找到span
	Span* MapObjectToSpan(void* obj);
	// 管理cc还回来的span
	void ReleaseSpanToPageCache(Span* span);
private:
	// 构造函数私有化，并去掉拷贝构造和赋值
	PageCache() {}
	PageCache(const PageCache& other) = delete;
	PageCache& operator=(const PageCache& other) = delete;
};