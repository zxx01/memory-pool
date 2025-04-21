#include "../include/MemoryPool.h"

namespace Kama_memoryPool
{
    /**
     * @brief 构造函数
     * @param BlockSize 每个内存块的大小，默认为 4096 字节
     */
    MemoryPool::MemoryPool(size_t BlockSize)
        : BlockSize_(BlockSize), SlotSize_(0), firstBlock_(nullptr),
          curSlot_(nullptr), freeList_(nullptr), lastSlot_(nullptr)
    {
    }

    /**
     * @brief 析构函数
     * @details 释放所有分配的内存块。
     */
    MemoryPool::~MemoryPool()
    {
        // 把连续的block删除
        Slot *cur = firstBlock_;
        while (cur)
        {
            Slot *next = cur->next;
            // 等同于 free(reinterpret_cast<void*>(firstBlock_));
            // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
            operator delete(reinterpret_cast<void *>(cur));
            cur = next;
        }
    }

    /**
     * @brief 初始化内存池
     * @param size 每个槽的大小
     */
    void MemoryPool::init(size_t size)
    {
        assert(size > 0);
        SlotSize_ = size;
        firstBlock_ = nullptr;
        curSlot_ = nullptr;
        freeList_ = nullptr;
        lastSlot_ = nullptr;
    }

    /**
     * @brief 分配内存
     * @return 返回分配的内存指针
     */
    void *MemoryPool::allocate()
    {
        // 1. 优先尝试从“空闲链表”中取一个可复用的 slot
        Slot *slot = popFreeList();
        if (slot != nullptr)
            return slot;

        // 2. 如果没有可复用的 slot，就从“当前内存块”上分配
        Slot *temp;
        {
            // 2.1 在操作 curSlot_ / lastSlot_ 时加锁，
            //     保证同一时刻只有一个线程可以分配新 slot
            std::lock_guard<std::mutex> lock(mutexForBlock_);

            // 2.2 如果当前块用光了（curSlot_ 已越过 lastSlot_），申请新块
            if (curSlot_ >= lastSlot_)
            {
                allocateNewBlock();
            }

            // 2.3 在当前块上划出一个 slot
            temp = curSlot_;
            // 这里不能直接 curSlot_ += SlotSize_，
            // 因为 curSlot_ 是 Slot* 类型（64位系统中就 8 字节），所以需要除以 SlotSize_ 再加 1，
            // 以得到正确的跳转字节数。
            curSlot_ += SlotSize_ / sizeof(Slot);
        }

        return temp;
    }

    /**
     * @brief 释放内存
     * @param ptr 要释放的内存指针
     */
    void MemoryPool::deallocate(void *ptr)
    {
        if (!ptr)
            return;

        Slot *slot = reinterpret_cast<Slot *>(ptr);
        pushFreeList(slot);
    }

    /**
     * @brief 分配新的内存块
     * @details 当当前内存块不足时，申请新的内存块。
     */
    void MemoryPool::allocateNewBlock()
    {
        // std::cout << "申请一块内存块，SlotSize: " << SlotSize_ << std::endl;
        //  头插法插入新的内存块
        void *newBlock = operator new(BlockSize_);
        reinterpret_cast<Slot *>(newBlock)->next = firstBlock_;
        firstBlock_ = reinterpret_cast<Slot *>(newBlock);

        char *body = reinterpret_cast<char *>(newBlock) + sizeof(Slot *);
        size_t paddingSize = padPointer(body, SlotSize_); // 计算对齐需要填充内存的大小
        curSlot_ = reinterpret_cast<Slot *>(body + paddingSize);

        // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
        lastSlot_ = reinterpret_cast<Slot *>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);

        freeList_ = nullptr;
    }

    /**
     * @brief 指针对齐
     * @param p 要对齐的指针
     * @param align 对齐的字节数
     * @return 返回需要填充的字节数
     */
    size_t MemoryPool::padPointer(char *p, size_t align)
    {
        // align 是槽大小
        return (align - reinterpret_cast<size_t>(p)) % align;
    }

    /**
     * @brief 将槽加入空闲链表
     * @param slot 要加入的槽
     * @return 是否成功加入
     */
    bool MemoryPool::pushFreeList(Slot *slot)
    {
        while (true)
        {
            // 获取当前头节点
            Slot *oldHead = freeList_.load(std::memory_order_relaxed);
            // 将新节点的 next 指向当前头节点
            slot->next.store(oldHead, std::memory_order_relaxed);

            // 尝试将新节点设置为头节点
            // 为什么使用 memory_order_release ？
            // 当你把一个新的 slot 节点压到链表头上时，你不仅仅是在改写 freeList_ 指针，还隐含着：
            // - 你可能刚在这个 slot 上完成了对象的析构，
            // - 或者做了一些其他写入（比如 slot->next.store(oldHead)）。
            // - 这些写入都 必须在 “链表头被换成新节点” 这个动作之前完成，
            // - 才能保证后续取出此节点的线程能看到它们。
            if (freeList_.compare_exchange_weak(oldHead, slot,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
            {
                return true;
            }
            // 失败：说明另一个线程可能已经修改了 freeList_
            // CAS 失败则重试
        }
    }

    /**
     * @brief 从 freeList_ 中弹出一个空闲的 Slot 节点（用于对象分配）。
     *        这是一个 无锁栈（lock-free stack） 的出栈操作，支持 并发多线程 安全访问。
     * @return 返回取出的槽指针
     */
    Slot *MemoryPool::popFreeList()
    {
        while (true)
        {
            // 1. 原子地读取 freeList_ 的当前头节点
            Slot *oldHead = freeList_.load(std::memory_order_acquire);
            // 2. 如果头为空，直接返回 nullptr（无可用槽）
            if (oldHead == nullptr)
                return nullptr;

            // 3. 计算“弹出”后新的头节点
            Slot *newHead;
            try
            {
                // 3.1 只读 oldHead->next，不需要同步语义
                newHead = oldHead->next.load(std::memory_order_relaxed);
            }
            catch (...)
            {
                // 3.2 防御式编程：极端情况下 retry
                continue;
            }

            // 4. 尝试原子地把 freeList_ 从 oldHead 更新为 newHead
            //    成功时以 acquire 语义，失败时以 relaxed 语义
            // 为什么使用acquire？
            // 当消费者线程成功把头节点从链表里弹出（CAS 成功） 时，
            // 它要读取并使用这个 oldHead，并且接下来要安全地访问 oldHead->next，
            // 甚至更可能在这块内存上构造新对象。要保证它看到的是“发布者”在 release 时
            // 写下的、完整初始化过的内存状态，就必须在 CAS 成功时用 acquire 语义
            if (freeList_.compare_exchange_weak(
                    oldHead,                    // 预期旧值
                    newHead,                    // 新值
                    std::memory_order_acquire,  // 成功：acquire
                    std::memory_order_relaxed)) // 失败：relaxed
            {
                // 5. CAS 成功：我们“独占”了 oldHead，返回它
                return oldHead;
            }
            // 6. CAS 失败：oldHead 被更新或发生伪失败，循环重试
        }
    }

    /**
     * @brief 初始化所有内存池
     * @details 根据槽大小初始化多个内存池。
     */
    void HashBucket::initMemoryPool()
    {
        for (int i = 0; i < MEMORY_POOL_NUM; i++)
        {
            // 每个内存池的槽大小为 (i + 1) * SLOT_BASE_SIZE
            // 例如：第一个内存池的槽大小为 1 * 8 = 8 字节，第二个内存池的槽大小为 2 * 8 = 16 字节
            getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
        }
    }

    /**
     * @brief 获取指定索引的内存池
     * @param index 内存池索引
     * @return 返回对应的内存池引用
     */
    MemoryPool &HashBucket::getMemoryPool(int index)
    {
        static MemoryPool memoryPool[MEMORY_POOL_NUM];
        return memoryPool[index];
    }

} // namespace memoryPool
