// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_BYTES_HPP
#define ML_PYROWAVE_BYTES_HPP

#include "MLPyrowavePayloadPool.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace MLPyrowave {
namespace detail {
template <typename T>
struct OverwriteAllocator : std::allocator<T> {
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;
    template <typename U> struct rebind { using other = OverwriteAllocator<U>; };
    std::shared_ptr<PayloadPool> pool;

    OverwriteAllocator() noexcept = default;
    explicit OverwriteAllocator(std::shared_ptr<PayloadPool> p) noexcept : pool(std::move(p)) {}
    template <typename U>
    OverwriteAllocator(const OverwriteAllocator<U> &other) noexcept : pool(other.pool) {}

    T *allocate(size_t n)
    {
        if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
        return pool ? static_cast<T *>(pool->acquire(n * sizeof(T))) :
                      std::allocator<T>::allocate(n);
    }
    void deallocate(T *p, size_t n)
    {
        if (pool) pool->release(p);
        else std::allocator<T>::deallocate(p, n);
    }

    template <typename U>
    void construct(U *p) { ::new (static_cast<void *>(p)) U; }

    template <typename U, typename Arg, typename... Args>
    void construct(U *p, Arg &&arg, Args &&...args)
    {
        ::new (static_cast<void *>(p)) U(std::forward<Arg>(arg),
                                       std::forward<Args>(args)...);
    }
};

template <typename T, typename U>
bool operator==(const OverwriteAllocator<T> &a, const OverwriteAllocator<U> &b) noexcept
{ return a.pool == b.pool; }
template <typename T, typename U>
bool operator!=(const OverwriteAllocator<T> &a, const OverwriteAllocator<U> &b) noexcept
{ return !(a == b); }
} // namespace detail

// Byte buffers only, never masks/offsets. Count construction and resize without
// a value start lifetimes but leave new bytes indeterminate. Explicitly write ALL
// readable payload bytes before validation, equality or GPU use, and guards
// before publication. Supply an explicit value when zero-fill is required.
// Move propagation preserves allocation and pool lifetime across vector moves.
using CodecBytes = std::vector<uint8_t, detail::OverwriteAllocator<uint8_t>>;
inline size_t CodecBytesAllocationSize(const CodecBytes &bytes)
{
    return bytes.get_allocator().pool ? PayloadPool::SlotBytes : bytes.capacity();
}
} // namespace MLPyrowave
#endif
