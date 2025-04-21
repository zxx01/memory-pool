## 按需向系统要页

在第一次分配某个尺寸类别时，ThreadCache 会在本地没块可用，就去 CentralCache 拿；CentralCache 又在自己的 free list 也空了，就调用 PageCache::allocateSpan。

对于小于等于 32 KB 的尺寸类别，PageCache 一次只按固定的 SPAN_PAGES=8（共 8 × 4 KB = 32 KB）向操作系统 (mmap) 申请；对于更大的请求，则按需页数申请。

因此最初系统调用只会申请这 32 KB（或按大对象实际需求的页数），并不会一次性申请成百上千页。

## 多余空间的缓存与重用

PageCache 拿回的 32 KB span 会被拆成多个块（block）后返还给 CentralCache。CentralCache 再拆成单个块后返给 ThreadCache，ThreadCache 只取一个满足本次分配的块，其余块留在本地 free list。

当这些块被用户释放时，又会先回到 ThreadCache，再根据阈值批量返还给 CentralCache。CentralCache 中的块累积到一定量，也可以整体再返还给 PageCache（调用 deallocateSpan），最终又变回一个或多个 span，放回 freeSpans_，供下次 mmap 前复用。

## 渐进式增长

系统 alloc 只在缓存全空时才触发，且一次量固定；

随着用户不断分配和释放，同一批 span/块 会被循环复用，真正的系统调用次数很少——只有在前面分配的 32 KB 枯竭后，才会再申请下一批。

这样就达到了 “只在必要时” 向操作系统要内存，而多余的空间则通过这三级缓存机制在各层之间不断流转、累积和复用，最大限度地降低了系统调用和碎片化。