// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_BYTES_HPP
#define ML_PYROWAVE_BYTES_HPP

#include <cstdint>
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
    template <typename U> struct rebind { using other = OverwriteAllocator<U>; };

    OverwriteAllocator() noexcept = default;
    template <typename U>
    OverwriteAllocator(const OverwriteAllocator<U> &) noexcept {}

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
bool operator==(const OverwriteAllocator<T> &, const OverwriteAllocator<U> &) noexcept
{ return true; }
template <typename T, typename U>
bool operator!=(const OverwriteAllocator<T> &, const OverwriteAllocator<U> &) noexcept
{ return false; }
} // namespace detail

// Byte buffers only, never masks/offsets. Count construction and resize without
// a value start lifetimes but leave new bytes indeterminate. Explicitly write ALL
// readable payload bytes before validation, equality or GPU use, and guards
// before publication. Supply an explicit value when zero-fill is required.
// Stateless, always-equal allocators preserve the allocation across vector moves.
using CodecBytes = std::vector<uint8_t, detail::OverwriteAllocator<uint8_t>>;
} // namespace MLPyrowave
#endif
