// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_PAYLOAD_POOL_HPP
#define ML_PYROWAVE_PAYLOAD_POOL_HPP

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>

namespace MLPyrowave {
// Allocators retain shared ownership through every lease; destruction therefore
// occurs only after the last vector releases its slot. Payload is never cleared.
class PayloadPool final {
public:
    // Use these constants by value (not reference) to avoid C++14 ODR definitions.
    static constexpr size_t SlotCount = 3, Alignment = 64u * 1024u;
    static constexpr size_t SlotBytes = 16u * 1024u * 1024u + Alignment;
    static constexpr size_t MaxBytes = SlotCount * SlotBytes;

    PayloadPool() = default;
    PayloadPool(const PayloadPool &) = delete;
    PayloadPool &operator=(const PayloadPool &) = delete;
    ~PayloadPool() { for (auto &slot : slots_) std::free(slot.data); }

    void *acquire(size_t bytes)
    {
        if (bytes > SlotBytes) throw std::bad_alloc();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &slot : slots_) {
            if (slot.used) continue;
            if (!slot.data && ::posix_memalign(&slot.data, Alignment, SlotBytes) != 0)
                throw std::bad_alloc();
            slot.used = true;
            return slot.data;
        }
        throw std::bad_alloc(); // No growth or fallback when all three are leased.
    }

    void release(void *p)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &slot : slots_) {
            if (slot.data == p) { slot.used = false; return; }
        }
    }

private:
    struct Slot { void *data = nullptr; bool used = false; };
    Slot slots_[SlotCount];
    std::mutex mutex_;
};
} // namespace MLPyrowave
#endif
