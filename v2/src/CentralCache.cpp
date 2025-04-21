#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>

namespace Kama_memoryPool
{

    // 每次从PageCache获取span大小（以页为单位）
    static const size_t SPAN_PAGES = 8;

    /**
     * @brief 从中心缓存中获取一批内存块。
     *
     * 根据指定的索引，从中心缓存中获取一批内存块。如果中心缓存为空，则从页缓存中获取新的内存块，
     * 并将其切分成小块后存入中心缓存。获取的第一块内存块返回给调用者，其余部分存入中心缓存。
     *
     * @param index 内存块大小对应的索引。
     * @return void* 返回分配的内存块指针，如果获取失败则返回 nullptr。
     */
    void *CentralCache::fetchRange(size_t index)
    {
        /*
        自旋锁：atomic_flag + yield 轻量、高并发场景下竞争少时效率更优。

        原子操作：
        - load(relaxed) 读取链表头，无需顺序保证，因为已由锁保护。
        - store(release) 发布新的链表头，保证前面对链表节点写入的可见性。

        批量分割：当中心缓存空时，一次大块拆分成多小块，摊薄系统调用与锁开销，提高吞吐。
        */

        // 1. 边界检查
        // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大，应直接向系统申请
        if (index >= FREE_LIST_SIZE)
            return nullptr;

        // 2. 自旋锁获取
        // 自旋锁保护
        while (locks_[index].test_and_set(std::memory_order_acquire))
        {
            // 提示操作系统（或线程调度器）​当前线程自愿放弃剩余的 CPU 时间片，使其他就绪线程有机会执行。
            // 但是具体的实现依赖于操作系统和编译器。
            // 这并不是一个强制的操作，线程仍然可能会继续执行。
            // 这行代码的目的是为了避免忙等待（busy waiting），
            // 让出 CPU 时间片给其他线程，减少 CPU 的浪费。
            std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
        }

        void *result = nullptr;
        try
        {
            // 3. 从中心链表加载头节点
            // 尝试从中心缓存获取内存块
            // 在持锁状态下，无需额外同步，直接以 relaxed 方式读取当前链表头指针。
            result = centralFreeList_[index].load(std::memory_order_relaxed);

            // 4. 链表为空 → 从页缓存获取新 span
            if (!result)
            {
                // 如果中心缓存为空，从页缓存获取新的内存块
                size_t size = (index + 1) * ALIGNMENT;
                result = fetchFromPageCache(size);

                // 若系统 mmap 或页缓存都失败，则释放锁并返回 nullptr，分配请求宣告失败。
                if (!result)
                {
                    locks_[index].clear(std::memory_order_release);
                    return nullptr;
                }

                // 5. 拆分大块 span 为多个小块
                // 将获取的内存块切分成小块
                char *start = static_cast<char *>(result);
                size_t blockNum = (SPAN_PAGES * PageCache::PAGE_SIZE) / size;

                if (blockNum > 1)
                {
                    // 1. 构造单向链表：每个块首存放“下一个块”指针
                    // 确保至少有两个块才构建链表
                    for (size_t i = 1; i < blockNum; ++i)
                    {
                        void *current = start + (i - 1) * size;
                        void *next = start + i * size;
                        *reinterpret_cast<void **>(current) = next;
                    }
                    *reinterpret_cast<void **>(start + (blockNum - 1) * size) = nullptr;

                    // 保存result的下一个节点
                    void *next = *reinterpret_cast<void **>(result);
                    // 将result与链表断开
                    *reinterpret_cast<void **>(result) = nullptr;
                    // 更新中心缓存
                    centralFreeList_[index].store(
                        next,
                        std::memory_order_release);
                }
            }
            // 6. 链表非空 → 直接弹出一个节点
            else
            {
                // 保存result的下一个节点
                void *next = *reinterpret_cast<void **>(result);
                // 将result与链表断开
                *reinterpret_cast<void **>(result) = nullptr;

                // 更新中心缓存
                centralFreeList_[index].store(next, std::memory_order_release);
            }
        }
        catch (...) // 7. 异常安全 & 释放锁
        {
            locks_[index].clear(std::memory_order_release);
            throw;
        }

        // 释放锁
        locks_[index].clear(std::memory_order_release);
        return result;
    }

    /**
     * @brief 将一批内存块归还到中心缓存。
     *
     * 该函数将线程本地的内存块链表归还到中心缓存中。归还时会将链表的尾节点与中心缓存的链表头连接，
     * 并原子地更新中心缓存的链表头指针。
     *
     * @param start 要归还的内存块链表的起始指针。
     * @param size 要归还的内存块数量。
     * @param index 内存块大小对应的索引。
     */
    void CentralCache::returnRange(void *start, size_t size, size_t index)
    {
        /*
        锁粒度小：每个尺寸类别独占一个 atomic_flag；不同尺寸并发无干扰。

        原子操作优化：读用 relaxed，写用 release，只在必要环节加同步，减少内存屏障开销。

        侵入式链表拼接：O(1) 时间完成链表头部拼接，操作简单高效。
        */

        // （1）边界检查
        // index >= FREE_LIST_SIZE：超大对象不应进缓存。
        if (!start || index >= FREE_LIST_SIZE)
            return;

        // （2）获取自旋锁
        while (locks_[index].test_and_set(std::memory_order_acquire))
        {
            std::this_thread::yield(); // 让出 CPU，避免忙等待
        }

        try
        {
            // （3）找到返还链表的尾节点
            void *end = start;
            size_t count = 1;
            // 通过 next 指针遍历，count < size 用于防止超长或环形链表
            while (*reinterpret_cast<void **>(end) != nullptr && count < size)
            {
                end = *reinterpret_cast<void **>(end);
                ++count;
            }

            // （4）取出当前中心链表头
            void *current = centralFreeList_[index].load(std::memory_order_relaxed);

            // （5）将待返还链表尾接到 current 上
            *reinterpret_cast<void **>(end) = current;

            // （6）原子地把 start 设为新的链表头
            centralFreeList_[index].store(start, std::memory_order_release);
        }
        catch (...)
        {
            // 异常安全：确保释放锁后再抛出异常
            locks_[index].clear(std::memory_order_release);
            throw;
        }

        // （7）释放自旋锁
        locks_[index].clear(std::memory_order_release);
    }

    /**
     * @brief 从页缓存中获取内存块。
     *
     * 根据请求的内存块大小，从页缓存中分配内存块。如果请求的大小小于等于 32KB，
     * 则分配固定大小的 8 页内存块；如果请求的大小大于 32KB，则按实际需求分配页数。
     *
     * @param size 请求的内存块大小（字节）。
     * @return void* 返回分配的内存块指针，如果分配失败则返回 nullptr。
     */
    void *CentralCache::fetchFromPageCache(size_t size)
    {
        // 1. 计算实际需要的页数
        size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

        // 2. 根据大小决定分配策略
        if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
        {
            // 小于等于 32KB 的请求，使用固定 8 页
            return PageCache::getInstance().allocateSpan(SPAN_PAGES);
        }
        else
        {
            // 大于 32KB 的请求，按实际需求分配
            return PageCache::getInstance().allocateSpan(numPages);
        }
    }

} // namespace memoryPool