#ifndef OBJECTPOOL_H
#define OBJECTPOOL_H

#include "Common.h"

using std::cout;
using std::endl;


template<class T>
class ObjectPool
{
public:
	T* New() // 申请一个T类型大小的空间
	{
		T* obj = nullptr; // 最终返回的空间

		if (_freelist != nullptr)
		{ // _freelist不为空，表示有回收的T大小的小块可以重复利用
			void* next = *reinterpret_cast<void**>(_freelist);
			obj = reinterpret_cast<T*>(_freelist);
			_freelist = next;
			// 头删操作
		}
		else
		{ // 自由链表中没有块，也就没有可以重复利用的空间
			// _memory中剩余空间小于T的大小的时候再开空间
			if (_remanentBytes < sizeof(T)) // 这样也会包含剩余空间为0的情况
			{
				// _remanentBytes = 128 * 1024; // 开128K的空间
				// 如果 T 的大小很大(比如基数树的 Node, 256KB), 至少要申请足够容纳它的倍数
    			_remanentBytes = (sizeof(T) > 128 * 1024) ? (sizeof(T) * 2) : (128 * 1024);

				// 右移PAGE_SHIFT位，就是除以4/8KB，也就是得到的是16，这里就表示申请16页
				_memory = (char*)SystemAlloc(_remanentBytes >> PAGE_SHIFT);

				if (_memory == nullptr) // 开失败了抛异常
				{
					throw std::bad_alloc();
				}
			}

			obj = reinterpret_cast<T*>(_memory); // 给定一个T类型的大小
			// 判断一下T的大小，小于指针就给一个指针大小，大于指针就还是T的大小
			size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			_memory += objSize; // _memory后移一个T类型的大小
			_remanentBytes -= objSize; // 空间给出后_remanetBytes减少了T类型的大小
		}

		new(obj) T; // 通过定位new调用构造函数进行初始化

		return obj;
	}

	void Delete(T* obj) // 回收还回来的小空间
	{
		// 显示调用析构函数进行清理工作
		obj->~T();

		// 头插
		*reinterpret_cast<void**>(obj) = _freelist; // 新块指向旧块(或空)
		_freelist = obj; // 头指针指向新块
	}

private:
	char* _memory = nullptr; // 指向内存块的指针
	size_t _remanentBytes = 0; // 大块内存在切分过程中的剩余字节数
	void* _freelist = nullptr; // 自由链表，用来连接归还的空闲空间
public:
	std::mutex _poolMtx; // 防止ThreadCache申请时申请到空指针
};

#endif