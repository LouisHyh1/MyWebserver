#ifndef PAGEMAP_H
#define PAGEMAP_H

#include "Common.h"

// Three-level radix tree
// 适用于 64 位系统，因为 64 位系统的页号数量庞大，两层基数树无法有效节约内存
template <int BITS>
class TCMalloc_PageMap3 {
private:
	// 将总的 BITS 拆分到三层中。
	// 这里参考了 tcmalloc 的经典做法，将 BITS 大致平均分配给三层。
	// 例如在 64 位系统下，若页号占用 36 位，则可能分为 12, 12, 12
	static const int INTERIOR_BITS = (BITS + 2) / 3;
	static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;

	static const int LEAF_BITS = (BITS + 2) / 3;
	static const int LEAF_LENGTH = 1 << LEAF_BITS;

	// 根节点拿走剩下的比特位
	static const int ROOT_BITS = BITS - LEAF_BITS - INTERIOR_BITS;
	static const int ROOT_LENGTH = 1 << ROOT_BITS;

	// 第三层：叶子节点（存放最终映射的 void* 指针，通常是 Span*）
	struct Leaf {
		void* values[LEAF_LENGTH];
	};

	// 第二层：内部节点（存放指向第三层叶子节点的指针数组）
	struct Node {
		Leaf* ptrs[INTERIOR_LENGTH];
	};

	// 第一层：根（存放指向第二层内部节点的指针数组）
	Node* root_[ROOT_LENGTH];

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap3() {
		// 初始化根节点，全部置空
		memset(root_, 0, sizeof(root_));

		// 注意：对于 64 位系统，绝对不能在这里一次性将整个地址空间全开出来！
		// 否则会瞬间耗尽物理内存。
		// 三层基数树必须依赖于按需开辟空间，即在映射 Span 时才调用 Ensure 去分配对应路径上的节点。
		// PreallocateMoreMemory(); // 这里不要调用全量开辟
	}

	// 通过页号 k 获取对应的指针
	void* get(Number k) const {
		// 计算第一层（根节点）的下标
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		// 计算第二层（内部节点）的下标：先右移吃掉第三层的位，再用掩码取第二层的位
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		// 计算第三层（叶子节点）的下标：直接用掩码取最低的几位
		const Number i3 = k & (LEAF_LENGTH - 1);

		// 越界检查 或 第一层未分配 或 第二层未分配，均返回 NULL
		if ((k >> BITS) > 0 || root_[i1] == nullptr || root_[i1]->ptrs[i2] == nullptr) {
			return nullptr;
		}
		// 返回第三层中保存的值
		return root_[i1]->ptrs[i2]->values[i3];
	}

	// 将指针 val 设置到页号 k 对应的位置
	// val 通常是一个 Span*，但为了通用性，这里使用 void* 类型
	void set(Number k, void* val) {
		const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		assert(i1 < ROOT_LENGTH);
		// 确保在 set 之前，Ensure 已经开辟好了对应的路径节点
		assert(root_[i1] != nullptr);
		assert(root_[i1]->ptrs[i2] != nullptr);

		root_[i1]->ptrs[i2]->values[i3] = val;
	}

	// 确保从 start 开始往后的 n 页空间开好了（延迟按需分配的核心机制）
	bool Ensure(Number start, size_t n) {
		for (Number key = start; key <= start + n - 1;) {
			const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);

			// Check for overflow
			if (i1 >= ROOT_LENGTH)
				return false;

			// 1. 如果第一层指向第二层的节点（Node）没开，就通过对象池开空间
			if (root_[i1] == nullptr) {
				static ObjectPool<Node> nodePool;
				Node* node = nodePool.New();
				memset(node, 0, sizeof(*node));
				root_[i1] = node;
			}

			// 2. 如果第二层指向第三层的节点（Leaf）没开，也通过对象池开空间
			if (root_[i1]->ptrs[i2] == nullptr) {
				static ObjectPool<Leaf> leafPool;
				Leaf* leaf = leafPool.New();
				memset(leaf, 0, sizeof(*leaf));
				root_[i1]->ptrs[i2] = leaf;
			}

			// 将 key 直接步进跨过当前已被确保的整个 Leaf 范围
			// 这样可以避免一页一页循环判断，极大提高性能
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	// 在 64 位三层基数树中，我们放弃初始化时将空间“全开出来”的做法
	void PreallocateMoreMemory() {
		// 只做接口预留，不执行 Ensure(0, 1 << BITS);
		// 实际应用中，当系统向 OS 申请到一块大内存空间 (Span) 时，
		// 再针对那一段具体的范围调用 Ensure(start_page, page_count) 即可。
	}
};

#endif