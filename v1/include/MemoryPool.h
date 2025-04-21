#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace Kama_memoryPool
{
#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8
#define MAX_SLOT_SIZE 512

    /* 具体内存池的槽大小没法确定，因为每个内存池的槽大小不同(8的倍数)
       所以这个槽结构体的sizeof 不是实际的槽大小 */
    /**
     * @brief 内存槽结构体
     * @details 每个槽用于存储分配的内存块，使用链表结构管理。
     */
    struct Slot
    {
        std::atomic<Slot *> next; ///< 指向下一个槽的原子指针
    };

    /**
     * @brief 内存池类
     * @details 该类实现了一个简单的内存池，使用链表来管理内存块和槽。
     */
    class MemoryPool
    {
    public:
        /**
         * @brief 构造函数
         * @param BlockSize 每个内存块的大小，默认为 4096 字节
         */
        MemoryPool(size_t BlockSize = 4096);

        /**
         * @brief 析构函数
         * @details 释放所有分配的内存块。
         */
        ~MemoryPool();

        /**
         * @brief 初始化内存池
         * @param size 每个槽的大小
         */
        void init(size_t size);

        /**
         * @brief 分配内存
         * @return 返回分配的内存指针
         */
        void *allocate();

        /**
         * @brief 释放内存
         * @param ptr 要释放的内存指针
         */
        void deallocate(void *ptr);

    private:
        /**
         * @brief 分配新的内存块
         * @details 当当前内存块不足时，申请新的内存块。
         */
        void allocateNewBlock();

        /**
         * @brief 指针对齐
         * @param p 要对齐的指针
         * @param align 对齐的字节数
         * @return 返回需要填充的字节数
         */
        size_t padPointer(char *p, size_t align);

        /**
         * @brief 将槽加入空闲链表
         * @param slot 要加入的槽
         * @return 是否成功加入
         */
        bool pushFreeList(Slot *slot);

        /**
         * @brief 从空闲链表中取出一个槽
         * @return 返回取出的槽指针
         */
        Slot *popFreeList();

    private:
        int BlockSize_;                ///< 内存块大小
        int SlotSize_;                 ///< 槽大小
        Slot *firstBlock_;             ///< 指向内存池管理的首个实际内存块
        Slot *curSlot_;                ///< 指向当前第一个未被使用过的槽
        std::atomic<Slot *> freeList_; ///< 指向空闲的槽(被使用过后又被释放的槽)
        Slot *lastSlot_;               ///< 当前内存块中最后能够存放元素的位置标识(超过该位置需申请新的内存块)
        std::mutex mutexForBlock_;     ///< 保证多线程情况下避免重复开辟内存导致的浪费行为
    };

    /**
     * @brief 内存池集合管理类
     * @details 负责内存池集合管理与动态调度，支持多种槽大小。
     */
    class HashBucket
    {
    public:
        /**
         * @brief 初始化所有内存池
         * @details 根据槽大小初始化多个内存池。
         */
        static void initMemoryPool();

        /**
         * @brief 获取指定索引的内存池
         * @param index 内存池索引
         * @return 返回对应的内存池引用
         */
        static MemoryPool &getMemoryPool(int index);

        /**
         * @brief 分配内存
         * @details 根据槽大小分配内存，使用内存池或 new。
         *          如果槽大小大于512字节，则使用 new。
         *          如果槽大小小于等于512字节，则使用内存池分配。
         *          内存池的槽大小为 8 的倍数，分配时会向上取整到 8 的倍数。
         *          例如：分配 9 字节的内存，实际分配 16 字节的内存。
         * @param size 要分配的内存大小
         * @return 返回分配的内存指针
         */
        static void *useMemory(size_t size)
        {
            if (size <= 0)
                return nullptr;
            if (size > MAX_SLOT_SIZE) // 大于512字节的内存，则使用new
                return operator new(size);

            // 相当于size / 8 向上取整（因为分配内存只能大不能小
            return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
        }

        /**
         * @brief 释放内存
         * @param ptr 要释放的内存指针
         * @param size 内存大小
         */
        static void freeMemory(void *ptr, size_t size)
        {
            if (!ptr)
                return;

            // 如果大于512字节的内存，则使用 operator delete 释放，
            // 因为是用 operator new 分配的内存
            if (size > MAX_SLOT_SIZE)
            {
                operator delete(ptr);
                return;
            }

            // 相当于size / 8 向上取整（因为分配内存只能大不能小
            // 例如：分配 9 字节的内存，实际分配 16 字节的内存。
            // 释放时也要向上取整到 8 的倍数
            // 例如：释放 9 字节的内存，实际释放 16 字节的内存。
            getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
        }

        /**
         * @brief 分配并构造对象
         * @tparam T 对象类型
         * @tparam Args 构造函数参数类型
         * @param args 构造函数参数
         * @return 返回构造的对象指针
         */
        template <typename T, typename... Args>
        friend T *newElement(Args &&...args);

        /**
         * @brief 析构并释放对象
         * @tparam T 对象类型
         * @param p 要释放的对象指针
         */
        template <typename T>
        friend void deleteElement(T *p);
    };

    /**
     * @brief 分配并构造对象
     * @tparam T 对象类型
     * @tparam Args 构造函数参数类型
     * @param args 构造函数参数
     * @return 返回构造的对象指针
     */
    template <typename T, typename... Args>
    T *newElement(Args &&...args)
    {
        T *p = nullptr;
        // 根据元素大小选取合适的内存池分配内存
        if ((p = reinterpret_cast<T *>(HashBucket::useMemory(sizeof(T)))) != nullptr)
        {
            // 在分配的内存上构造对象
            new (p) T(std::forward<Args>(args)...);
        }

        return p;
    }

    /**
     * @brief 析构并释放对象
     * @tparam T 对象类型
     * @param p 要释放的对象指针
     */
    template <typename T>
    void deleteElement(T *p)
    {
        if (p)
        {
            // 对象析构
            p->~T();

            // 内存回收
            HashBucket::freeMemory(reinterpret_cast<void *>(p), sizeof(T));
        }
    }

} // namespace memoryPool
