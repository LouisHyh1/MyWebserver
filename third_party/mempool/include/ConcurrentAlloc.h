#pragma once
#include "ThreadCache.h"
#include "ObjectPool.h"
#include "PageCache.h"

// 其实就是tcmalloc，线程调用这个函数申请空间
void* ConcurrentAlloc(size_t size) {
	// 如果申请空间超过256KB，就直接找下层的去要
	if (size > MAX_BYTES) {
		size_t alignSize = SizeClass::RoundUp(size);  // 先按照页大小对齐
		size_t k = alignSize >> PAGE_SHIFT;  // 算出来对齐之后需要多少页

		PageCache::GetInstance()->_pageMtx.lock();  // 对pc中的span进行操作，加锁
		Span* span = PageCache::GetInstance()->NewSpan(k);  // 直接向pc要
		// 【核心修复】：必须将直接给用户的大 Span 标记为使用中！防误杀合并！
		span->_isUse = true;  // 将span标记为已使用
		span->_objSize = size;  // 记录申请的块大小
		PageCache::GetInstance()->_pageMtx.unlock();  // 解锁

		void* ptr = reinterpret_cast<void*>(span->_pageID << PAGE_SHIFT);  // 通过获得到的span来提供空间
		return ptr;
	}
	else {  // 申请空间小于256KB就走原先的逻辑
		// cout << std::this_thread::get_id() << " " << pTLSThreadCache << endl;
		// 因为pTSLThreadCache是TLS的，每个线程都会有一个且相互独立，所以不存在竞争问题
		// 因此只需要判断一次就可以直接new，不存在线程安全问题
		if (pTLSThreadCache == nullptr) {
			// 此时就相当于每个线程都有了一个ThreadCache对象

			// 用定长内存池来申请空间
			static ObjectPool<ThreadCache> objPool;  // 静态的，一直存在
			objPool._poolMtx.lock();  // 加锁，不然多线程可能会申请到空指针
			pTLSThreadCache = objPool.New();  // 申请空间
			objPool._poolMtx.unlock();
		}
		return pTLSThreadCache->Allocate(size);
	}
}

// 线程调用这个函数用来回收空间
void ConcurrentFree(void* obj) {
	assert(obj != nullptr);

	// 通过ptr找到对应的span，因为前面申请空间的
	// 时候已经保证了维护的空间首页地址已经映射过了
	Span* span = PageCache::GetInstance()->MapObjectToSpan(obj);
	size_t size = span->_objSize;  // 通过映射来的span获取obj所指向空间的大小

	// 通过size判断是不是大于256KB，是就走pc
	if (size > MAX_BYTES) {
		Span* span = PageCache::GetInstance()->MapObjectToSpan(obj);

		PageCache::GetInstance()->_pageMtx.lock();  // 对pc中的span进行操作，加锁
		PageCache::GetInstance()->ReleaseSpanToPageCache(span);  // 直接通过span释放空间
		PageCache::GetInstance()->_pageMtx.unlock();  // 解锁
	}
	else {  // 否则走tc
		pTLSThreadCache->Deallocate(obj, size);
	}
}