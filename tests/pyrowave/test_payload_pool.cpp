// SPDX-License-Identifier: GPL-3.0-only
// Standalone allocator checks: no codec/Common or GPU dependencies.
#include "MLPyrowaveBytes.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)
using namespace MLPyrowave;
using Alloc = CodecBytes::allocator_type;
template <typename F> static void rejects(F f)
{
    bool caught = false;
    try { f(); } catch (const std::bad_alloc &) { caught = true; }
    CHECK(caught);
}
static void testSlots()
{
    auto pool = std::make_shared<PayloadPool>();
    Alloc a(pool);
    rejects([&] { (void)pool->acquire(PayloadPool::SlotBytes + 1); });
    rejects([&] { (void)a.allocate(PayloadPool::SlotBytes + 1); });
    std::allocator_traits<Alloc>::rebind_alloc<uint64_t> wide(a);
    CHECK(wide.pool == pool && wide == a);
    rejects([&] { (void)wide.allocate(std::numeric_limits<size_t>::max() / 8 + 1); });
    detail::OverwriteAllocator<uint64_t> plain;
    rejects([&] { (void)plain.allocate(std::numeric_limits<size_t>::max() / 8 + 1); });
    CodecBytes first(40, 0x11, a), second(40, 0x22, a);
    auto third = std::unique_ptr<CodecBytes>(new CodecBytes(PayloadPool::SlotBytes, 0x33, a));
    CHECK(first.data() != second.data() && first.data() != third->data());
    CHECK(second.data() != third->data());
    for (auto p : {first.data(), second.data(), third->data()})
        CHECK(reinterpret_cast<uintptr_t>(p) % PayloadPool::Alignment == 0);
    CHECK(CodecBytesAllocationSize(first) == PayloadPool::SlotBytes);
    CHECK(PayloadPool::MaxBytes == 3 * PayloadPool::SlotBytes);
    rejects([&] { CodecBytes fourth(1, a); });
    auto *recycled = third->data(); third.reset();
    CodecBytes reused(40, 0x44, a); CHECK(reused.data() == recycled);
    CHECK(std::all_of(first.begin(), first.end(), [](uint8_t b) { return b == 0x11; }));
    CHECK(std::all_of(second.begin(), second.end(), [](uint8_t b) { return b == 0x22; }));
    CHECK(std::all_of(reused.begin(), reused.end(), [](uint8_t b) { return b == 0x44; }));
}
static void testPropagation()
{
    static_assert(Alloc::propagate_on_container_move_assignment::value, "move propagation");
    static_assert(!Alloc::is_always_equal::value, "pool identity matters");
    auto pool = std::make_shared<PayloadPool>(), other = std::make_shared<PayloadPool>();
    Alloc a(pool), b(other), plain;
    CHECK(a == Alloc(pool) && a != b && a != plain && plain == Alloc());
    Alloc copied = a; CHECK(copied == a);
    CodecBytes source(24, 0x5a, a), copy = source;
    CHECK(copy == source && copy.data() != source.data() && copy.get_allocator() == a);
    const auto *p = source.data();
    CodecBytes moved(std::move(source)); CHECK(moved.data() == p && moved.get_allocator() == a);
    CodecBytes destination(8, 0, b);
    destination = std::move(moved);
    CHECK(destination.data() == p && destination.get_allocator() == a);
    CodecBytes normal(8, 0);
    CHECK(!normal.get_allocator().pool && CodecBytesAllocationSize(normal) == normal.capacity());
    CHECK(std::all_of(normal.begin(), normal.end(), [](uint8_t x) { return x == 0; }));
    normal = std::move(destination); CHECK(normal.data() == p && normal.get_allocator() == a);
    CodecBytes assigned(8, 0, b); assigned = copy;
    CHECK(assigned == copy && assigned.get_allocator() == b); // Copy assignment does not propagate.
}
static void testLifetime()
{
    auto pool = std::make_shared<PayloadPool>();
    std::weak_ptr<PayloadPool> weak = pool;
    auto bytes = std::unique_ptr<CodecBytes>(new CodecBytes(40, 0x6a, Alloc(pool)));
    pool.reset(); CHECK(!weak.expired());
    CHECK(std::all_of(bytes->begin(), bytes->end(), [](uint8_t b) { return b == 0x6a; }));
    bytes.reset(); CHECK(weak.expired()); // Deallocate precedes allocator's last shared_ptr release.
}
static void testConcurrent()
{
    auto pool = std::make_shared<PayloadPool>();
    CodecBytes held1(40, 0x11, Alloc(pool)), held2(40, 0x22, Alloc(pool));
    std::atomic<unsigned> active{0}, successes{0};
    auto worker = [&] {
        for (unsigned i = 0; i < 400; ++i) {
            try {
                CodecBytes bytes(128, Alloc(pool)); // Only the third slot can be leased.
                CHECK(active.fetch_add(1) == 0);
                std::fill(bytes.begin(), bytes.end(), uint8_t(i));
                std::this_thread::yield();
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                    [i](uint8_t b) { return b == uint8_t(i); }));
                CHECK(active.fetch_sub(1) == 1); ++successes;
            } catch (const std::bad_alloc &) { std::this_thread::yield(); }
        }
    };
    std::thread a(worker), b(worker); a.join(); b.join();
    CHECK(successes > 0 && active == 0);
    CHECK(std::all_of(held1.begin(), held1.end(), [](uint8_t x) { return x == 0x11; }));
    CHECK(std::all_of(held2.begin(), held2.end(), [](uint8_t x) { return x == 0x22; }));
    CodecBytes reused(40, 0, Alloc(pool)); CHECK(reused.size() == 40);
}
int main()
{
    testSlots(); testPropagation(); testLifetime(); testConcurrent();
    std::puts("payload pool standalone checks passed");
}
