#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace Kama_memoryPool
{

    /**
     * @brief 分配指定页数的内存块。
     *
     * 从 PageCache 中分配指定页数的内存块。如果有合适的空闲块，则直接分配；
     * 如果空闲块大于所需页数，则拆分多余部分；如果没有合适的空闲块，则向系统申请新的内存。
     *
     * @param numPages 请求分配的页数。
     * @return void* 返回分配的内存块起始地址，如果分配失败则返回 nullptr。
     */
    void *PageCache::allocateSpan(size_t numPages)
    {
        // 整个函数以上锁 mutex_ 开始，保证对 freeSpans_ 和 spanMap_ 的并发安全访问。
        std::lock_guard<std::mutex> lock(mutex_);

        // 1. 在 freeSpans_ 中查找第一个页数 ≥ numPages 的空闲 span
        auto it = freeSpans_.lower_bound(numPages);
        if (it != freeSpans_.end())
        {
            Span *span = it->second;

            // 2. 将该 span 从 freeSpans_ 链表中移除
            if (span->next)
            {
                freeSpans_[it->first] = span->next;
            }
            else
            {
                freeSpans_.erase(it);
            }

            // 3. 如果找到的 span 大于所需页数，则拆分出多余部分
            if (span->numPages > numPages)
            {
                Span *newSpan = new Span;
                newSpan->pageAddr = static_cast<char *>(span->pageAddr) + numPages * PAGE_SIZE;
                newSpan->numPages = span->numPages - numPages;
                newSpan->next = nullptr;

                // 3.1 将拆分出的尾部 newSpan 插入到 freeSpans_[剩余页数] 的链表头
                auto &list = freeSpans_[newSpan->numPages];
                newSpan->next = list;
                list = newSpan;

                // 3.2 原 span 缩减到正好 numPages
                span->numPages = numPages;
            }

            // 4. 记录 spanMap_，以便后续 deallocate 时能找到对应的 Span* 元数据
            spanMap_[span->pageAddr] = span;

            // 5. 返回这段正好或拆分后的连续页起始地址
            return span->pageAddr;
        }

        // 6. 如果没有合适的空闲 span，就向系统申请
        void *memory = systemAlloc(numPages);
        if (!memory)
            return nullptr;

        // 7. 用新的地址构造一个 Span，记录到 spanMap_ 并返回
        Span *span = new Span;
        span->pageAddr = memory;
        span->numPages = numPages;
        span->next = nullptr;
        spanMap_[memory] = span;
        return memory;
    }

    /**
     * @brief 释放指定页数的内存块。
     *
     * 将指定的内存块归还到 PageCache 中。如果可能，尝试合并相邻的空闲块以减少碎片。
     *
     * @param ptr 要释放的内存块起始地址。
     * @param numPages 要释放的页数。
     */
    void PageCache::deallocateSpan(void *ptr, size_t numPages)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 1. 在 spanMap_ 中查找对应的 Span* 元数据；若不存在，说明不是该缓存分配的内存，直接返回
        auto it = spanMap_.find(ptr);
        if (it == spanMap_.end())
            return;
        Span *span = it->second;

        // 2. 计算 ptr 之后紧邻的内存地址 nextAddr，看是否有可合并的相邻 span
        void *nextAddr = static_cast<char *>(ptr) + numPages * PAGE_SIZE;
        auto nextIt = spanMap_.find(nextAddr);

        if (nextIt != spanMap_.end())
        {
            Span *nextSpan = nextIt->second;
            bool found = false;
            // 3. 检查 nextSpan 是否在 freeSpans_[nextSpan->numPages] 的链表中
            auto &nextList = freeSpans_[nextSpan->numPages];

            // 3.1 若它正好是链表头，则直接弹出
            if (nextList == nextSpan)
            {
                nextList = nextSpan->next;
                found = true;
            }
            else if (nextList) // 3.2 否则遍历链表找到并摘除该节点
            {
                Span *prev = nextList;
                while (prev->next)
                {
                    if (prev->next == nextSpan)
                    {
                        prev->next = nextSpan->next;
                        found = true;
                        break;
                    }
                    prev = prev->next;
                }
            }

            // 4. 只有当 nextSpan 真正空闲且已摘除后，才与之合并
            if (found)
            {
                // 4.1 扩展当前 span 的页数
                span->numPages += nextSpan->numPages;
                // 4.2 从 map 中移除 nextSpan 的记录，并 delete 掉它
                spanMap_.erase(nextAddr);
                delete nextSpan;
            }
        }

        // 5. 将（可能已合并过的）span 放回 freeSpans_[span->numPages] 链表头
        auto &list = freeSpans_[span->numPages];
        span->next = list;
        list = span;
    }

    /**
     * @brief 向系统申请指定页数的内存。
     *
     * 使用 mmap 向操作系统申请内存，并清零返回的内存块。
     *
     * @param numPages 请求的页数。
     * @return void* 返回分配的内存块起始地址，如果分配失败则返回 nullptr。
     */
    void *PageCache::systemAlloc(size_t numPages)
    {
        size_t size = numPages * PAGE_SIZE;

        // 使用 mmap 分配内存
        void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED)
            return nullptr;

        // 清零内存
        memset(ptr, 0, size);
        return ptr;
    }

} // namespace memoryPool