// memoryutil.h — 显式卸载大容器时释放其主存储。
//
// vector::clear() 只销毁元素,允许保留 capacity 供下次复用。这适合筛选/重绘等热路径,
// 但不适合用户明确“关闭日志”或用新日志替换旧日志的边界:此时旧容量会让大日志的
// 内存长期留在进程里。与空容器 swap 后,旧主存储随临时对象析构而释放给分配器。
#pragma once

#include <vector>

namespace dl {

template <typename T, typename Alloc>
void releaseVector(std::vector<T, Alloc>& values) {
    std::vector<T, Alloc> empty(values.get_allocator());
    values.swap(empty);
}

} // namespace dl
