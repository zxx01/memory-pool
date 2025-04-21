#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"

namespace Kama_memoryPool
{
    /**
     * @brief 分配内存块。
     *
     * 根据请求的大小分配内存块。如果请求的大小为 0，则分配一个对齐大小的内存块。
     * 如果请求的大小超过 MAX_BYTES，则直接调用系统的 malloc 进行分配。
     * 否则，尝试从线程本地的自由链表中分配内存块。如果自由链表为空，则从中心缓存获取一批内存块。
     *
     * @param size 请求分配的内存块大小。
     * @return void* 返回分配的内存块指针。
     */
    void *ThreadCache::allocate(size_t size)
    {
        // 处理0大小的分配请求
        if (size == 0)
        {
            size = ALIGNMENT; // 至少分配一个对齐大小
        }

        if (size > MAX_BYTES)
        {
            // 大对象直接从系统分配
            return malloc(size);
        }

        size_t index = SizeClass::getIndex(size);

        // 更新自由链表大小
        freeListSize_[index]--;

        // 检查线程本地自由链表
        // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
        if (void *ptr = freeList_[index])
        {
            freeList_[index] = *reinterpret_cast<void **>(ptr); // 将freeList_[index]指向的内存块的下一个内存块地址（取决于内存块的实现）
            return ptr;
        }

        // 如果线程本地自由链表为空，则从中心缓存获取一批内存
        return fetchFromCentralCache(index);
    }

    /**
     * @brief 释放内存块。
     *
     * 根据内存块的大小，将其归还到线程本地的自由链表中。如果内存块大小超过 MAX_BYTES，
     * 则直接调用系统的 free 释放内存。对于较小的内存块，插入到对应的自由链表中，并更新链表大小。
     * 如果自由链表的大小超过设定的阈值，则将部分内存块归还给中心缓存。
     *
     * @param ptr 要释放的内存块指针。
     * @param size 要释放的内存块大小。
     */
    void ThreadCache::deallocate(void *ptr, size_t size)
    {
        // 如果内存块大小超过 MAX_BYTES，直接调用系统的 free 释放内存
        if (size > MAX_BYTES)
        {
            free(ptr);
            return;
        }

        size_t index = SizeClass::getIndex(size);

        // 插入到线程本地自由链表
        *reinterpret_cast<void **>(ptr) = freeList_[index];
        freeList_[index] = ptr;

        // 更新自由链表大小
        freeListSize_[index]++; // 增加对应大小类的自由链表大小

        // 判断是否需要将部分内存回收给中心缓存
        if (shouldReturnToCentralCache(index))
        {
            returnToCentralCache(freeList_[index], size);
        }
    }

    /**
     * @brief 判断是否需要将内存回收给中心缓存。
     *
     * 根据自由链表的大小判断是否需要将部分内存块归还给中心缓存。
     * 当自由链表的大小超过设定的阈值时，返回 true，表示需要回收。
     *
     * @param index 自由链表的索引。
     * @return true 如果自由链表大小超过阈值，返回 true。
     * @return false 如果自由链表大小未超过阈值，返回 false。
     */
    bool ThreadCache::shouldReturnToCentralCache(size_t index)
    {
        // 设定阈值，例如：当自由链表的大小超过一定数量时
        size_t threshold = 64; // 例如，64个内存块
        return (freeListSize_[index] > threshold);
    }

    /**
     * @brief 从中心缓存获取一批内存块。
     *
     * 该函数从中心缓存中获取一批指定大小的内存块，并将其拆分为单个块。
     * 第一个块返回给调用者，其余块存入线程本地的自由链表中。
     *
     * @param index 内存块大小对应的索引。
     * @return void* 返回分配的内存块指针，如果获取失败则返回 nullptr。
     */
    void *ThreadCache::fetchFromCentralCache(size_t index)
    {
        // 1. 向 CentralCache 请求一批该尺寸的内存块链表
        void *start = CentralCache::getInstance().fetchRange(index);
        if (!start)
            return nullptr;

        // 2. 将返回的链表拆分：第一个块留作本次 allocate 的返回值
        void *result = start;

        // 3. 将余下的所有块接入本地 free list（线程缓存）
        //    这里假设每个块的起始地址存储了“下一个块”的指针 (侵入式单向链表)
        freeList_[index] = *reinterpret_cast<void **>(start);

        // 4. 遍历计数，统计这次 fetchRange 拿到了多少个块
        // 更新自由链表大小
        size_t batchNum = 0;
        void *current = start; // 从start开始遍历

        // 计算从中心缓存获取的内存块数量
        while (current != nullptr)
        {
            batchNum++;
            current = *reinterpret_cast<void **>(current); // 遍历下一个内存块
        }

        // 5. 更新本地 freeList 的块数量统计
        freeListSize_[index] += batchNum;

        // 6. 返回第一个块给上层 allocate 调用
        return result;
    }

    /**
     * @brief 将多余的内存块归还给中心缓存。
     *
     * 当线程本地的自由链表中某个大小类别的内存块数量超过阈值时，
     * 将多余的内存块归还给中心缓存，以减少内存占用。
     *
     * @param start 自由链表的起始内存块指针。
     * @param size 要归还的内存块大小。
     */
    void ThreadCache::returnToCentralCache(void *start, size_t size)
    {
        // 根据大小计算对应的索引
        size_t index = SizeClass::getIndex(size);

        // 获取对齐后的实际块大小
        size_t alignedSize = SizeClass::roundUp(size);

        // 计算要归还内存块数量
        size_t batchNum = freeListSize_[index];
        if (batchNum <= 1)
            return; // 如果只有一个块，则不归还

        // 保留一部分在ThreadCache中（比如保留1/4）
        size_t keepNum = std::max(batchNum / 4, size_t(1));
        size_t returnNum = batchNum - keepNum;

        // 将内存块串成链表
        char *current = static_cast<char *>(start);
        // 使用对齐后的大小计算分割点
        char *splitNode = current;
        for (size_t i = 0; i < keepNum - 1; ++i)
        {
            splitNode = reinterpret_cast<char *>(*reinterpret_cast<void **>(splitNode));
            if (splitNode == nullptr)
            {
                // 如果链表提前结束，更新实际的返回数量
                returnNum = batchNum - (i + 1);
                break;
            }
        }

        if (splitNode != nullptr)
        {
            // 将要返回的部分和要保留的部分断开
            void *nextNode = *reinterpret_cast<void **>(splitNode);
            *reinterpret_cast<void **>(splitNode) = nullptr; // 断开连接

            // 更新ThreadCache的空闲链表
            freeList_[index] = start;

            // 更新自由链表大小
            freeListSize_[index] = keepNum;

            // 将剩余部分返回给CentralCache
            if (returnNum > 0 && nextNode != nullptr)
            {
                CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
            }
        }
    }

} // namespace memoryPool