#pragma once
#include "Common.h"

namespace Kama_memoryPool
{

    // 线程本地缓存
    class ThreadCache
    {
    public:
        /**
         * @brief 获取线程本地的 ThreadCache 实例。
         *
         * 该函数返回一个线程本地的 ThreadCache 实例，用于管理当前线程的内存分配和释放。
         * 使用 `thread_local` 关键字确保每个线程都有独立的实例，避免多线程竞争。
         *
         * @return ThreadCache* 返回当前线程的 ThreadCache 实例指针。
         */
        static ThreadCache *getInstance()
        {
            static thread_local ThreadCache instance;
            return &instance;
        }

        void *allocate(size_t size);
        void deallocate(void *ptr, size_t size);

    private:
        ThreadCache()
        {
            // 初始化自由链表和大小统计
            freeList_.fill(nullptr);
            freeListSize_.fill(0);
        }

        // 从中心缓存获取内存
        void *fetchFromCentralCache(size_t index);
        // 归还内存到中心缓存
        void returnToCentralCache(void *start, size_t size);

        bool shouldReturnToCentralCache(size_t index);

    private:
        // 每个线程的自由链表数组
        std::array<void *, FREE_LIST_SIZE> freeList_;
        std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计
    };

} // namespace memoryPool