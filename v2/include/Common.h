#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace Kama_memoryPool
{
    // 对齐数和大小定义
    constexpr size_t ALIGNMENT = 8;                          // 所有小于等于 256 KB 的分配都会向上对齐到 8 字节边界。
    constexpr size_t MAX_BYTES = 256 * 1024;                 // 256KB 超过该阈值的分配会直接调用系统分配（malloc/free）
    constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT 等于指针 void* 的大小

    // 内存块头部信息
    struct BlockHeader
    {
        size_t size;       // 内存块大小
        bool inUse;        // 使用标志
        BlockHeader *next; // 指向下一个内存块
    };

    // 大小类管理
    class SizeClass
    {
    public:
        /**
         * @brief 将给定的字节数向上对齐到指定的对齐边界。
         *
         * @param bytes 要对齐的字节数。
         * @return size_t 返回对齐后的字节数。
         */
        static size_t roundUp(size_t bytes)
        {
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        /**
         * @brief 计算给定字节数对应的索引。
         *
         * 此函数用于将字节数映射到内存池的索引位置，方便管理不同大小的内存块。
         * 首先确保字节数不小于对齐边界（ALIGNMENT），然后通过向上取整计算出对应的索引。
         *
         * @param bytes 要计算索引的字节数。
         * @return size_t 返回对应的索引值。
         */
        static size_t getIndex(size_t bytes)
        {
            // 确保bytes至少为ALIGNMENT
            bytes = std::max(bytes, ALIGNMENT);
            // 向上取整后-1
            return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
        }
    };

} // namespace memoryPool