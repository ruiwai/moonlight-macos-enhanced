// Synthetic CPU literals/XOR from test_receiver.cpp, NOT GPU encoder fixtures.
#import "MLPyrowaveCoordinator.h"
#include "MLPyrowaveReceiver.hpp"
#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)
static const std::array<uint8_t, 24> Codec = {{0x7f, 0xc0, 0x1f, 0xd0, 1, 0, 0, 0,
    1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0}};
static const std::array<uint8_t, 10> Latency = {{1, 0, 0, 5, 0, 0, 0, 2, 0x12, 0x34}};
static constexpr uint32_t Id = 0x12345678, Inner = 0x87654321, Outer = 0xabcdef01;
struct Packet { std::array<uint8_t, 256> bytes = {}; size_t length = 0; };
struct Shards { std::array<Packet, 4> packets; uint16_t count = 0; uint32_t id = 0; };
struct Clock { uint64_t now = 100; std::array<uint64_t, 4> script = {}; size_t count = 0, i = 0; };
static uint64_t tick(void *p)
{
    auto &c = *static_cast<Clock *>(p); return c.i < c.count ? c.script[c.i++] : c.now;
}
static uint64_t constantClock(void *) { return 100; } // Thread-safe, no mutable state.
static MLPyrowaveConfiguration config(uint64_t generation = 1)
{
    return {128, 128, 256, 0, 0, generation};
}
static MLPyrowaveCoordinator *create(Clock &c, uint64_t generation = 1)
{
    auto *r = [[MLPyrowaveCoordinator alloc] initWithConfiguration:config(generation)
                                                           clock:tick context:&c];
    CHECK(r); return r;
}
// Actual Common serializer, padded B=16 DATA plus one XOR parity shard.
static Shards build(uint32_t id = Id, const uint8_t *codec = Codec.data())
{
    std::array<uint8_t, 48> bytes = {};
    std::memcpy(bytes.data(), Latency.data(), Latency.size());
    std::memcpy(bytes.data() + Latency.size(), codec, Codec.size());
    Shards s; s.id = id; s.count = 3; std::array<uint8_t, 16> parity = {};
    for (uint16_t i = 0; i <= s.count; ++i) {
        LI_PYROWAVE_PACKET_HEADER h = {};
        h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
        h.frameId = id; h.rtpTimestamp = Inner;
        h.packetKind = i == s.count ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
        h.dataBlockCount = s.count; h.parityBlockCount = 1; h.blockCount = s.count + 1;
        h.blockIndex = i; h.fecBlockPayloadSize = h.payloadLength = 16;
        h.codecPayloadLength = uint32_t(Codec.size()); h.metadataLength = uint16_t(Latency.size());
        h.protectedPayloadLength = uint32_t(Codec.size() + Latency.size());
        h.metadataFlags = LI_PYROWAVE_METADATA_FLAG_PROTECTED;
        h.flags = LI_PYROWAVE_FLAG_METADATA_PRESENT;
        h.fecScheme = LI_PYROWAVE_FEC_SCHEME_XOR; h.fecDataCount = uint8_t(s.count);
        h.fecParityCount = 1; h.fecShardIndex = uint8_t(i);
        if (i == s.count) h.flags |= LI_PYROWAVE_FLAG_FEC_PARITY;
        else {
            if (i == 0) h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
            if (i + 1 == s.count) h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
        }
        const uint8_t *body = i == s.count ? parity.data() : bytes.data() + i * 16;
        auto &p = s.packets[i];
        CHECK(LiPyrowaveBuildPacket(&h, body, 16, p.bytes.data(), p.bytes.size(), &p.length) ==
              LI_PYROWAVE_PACKET_OK);
        if (i < s.count) for (size_t j = 0; j < 16; ++j) parity[j] ^= body[j];
    }
    return s;
}
static int send(MLPyrowaveCoordinator *r, const Packet &p, uint32_t id = Id,
                uint64_t arrival = 100)
{
    const auto before = p.bytes;
    LI_PYROWAVE_RECEIVED_PACKET packet = {p.bytes.data(), p.length, id, Outer, arrival, 9000};
    const int result = [r submitPacket:&packet];
    CHECK(before == p.bytes); return result;
}
static int deliver(MLPyrowaveCoordinator *r, const Shards &s, uint64_t arrival = 100)
{
    CHECK(send(r, s.packets[2], s.id, arrival) == 0);
    CHECK(send(r, s.packets[3], s.id, arrival) == 0); // Recover missing DATA[0] with XOR.
    return send(r, s.packets[1], s.id, arrival);
}
static void validate(MLPyrowaveFrame *frame, uint32_t id, uint64_t generation)
{
    CHECK(frame && frame.generation == generation);
    const auto *v = [frame validatedFrame]; CHECK(v && v->codec);
    const auto &f = v->codec->frameInfo();
    CHECK(f.frameId == id && f.rtpTimestamp == Inner && v->outerRtpTimestamp == Outer);
    CHECK(v->presentationTimeUs == 9000 && f.firstReceiveUs == 100 && f.deadlineUs == 100100);
    CHECK(f.metadata.hasHostProcessingLatency && f.metadata.hostProcessingLatency == 0x1234);
    CHECK(v->codec->payloadSize() == 24 && v->codec->bytes().size() == 40);
    CHECK(std::equal(Codec.begin(), Codec.end(), v->codec->bytes().begin()));
    CHECK(std::all_of(v->codec->bytes().begin() + 24, v->codec->bytes().end(),
                      [](uint8_t b) { return b == 0; }));
    CHECK(v->codec->wordOffsets().size() == 51 && v->codec->wordOffsets()[0] == 2);
}
static void testFactory()
{
    Clock c;
    CHECK(![[MLPyrowaveCoordinator alloc] initWithConfiguration:config() clock:nullptr context:&c]);
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto s = config();
        if (fault == 0) s.generation = 0;
        if (fault == 1) s.width = 127;
        if (fault == 2) s.height = 0;
        if (fault == 3) s.range = 2;
        if (fault == 4) s.siting = 2;
        if (fault == 5) s.maxPacketSize = 128;
        if (fault == 6) s.maxPacketSize = 65537;
        CHECK(![[MLPyrowaveCoordinator alloc] initWithConfiguration:s clock:tick context:&c]);
    }
}
static void testOwnershipAndSlot()
{
    Clock c; __strong MLPyrowaveFrame *retained = nil; // Borrowed clock outlives pool.
    __weak MLPyrowaveCoordinator *witness = nil;
    @autoreleasepool {
        auto *r = create(c); witness = r;
        {
            auto s = build(); CHECK(deliver(r, s) == 1);
            for (auto &p : s.packets) p.bytes.fill(0); // Source overwritten then destroyed.
        }
        retained = [r tryTakeFrame]; validate(retained, Id, 1); CHECK(![r tryTakeFrame]);
        CHECK(deliver(r, build(Id + 1)) == 1); CHECK(deliver(r, build(Id + 2)) == 1);
        CHECK([r submitPacket:nullptr] == 0);
        auto bad = build(Id + 3); bad.packets[0].bytes[0] ^= 1;
        CHECK(send(r, bad.packets[0], bad.id) == 0);
        auto codec = Codec; codec[4] = 2; CHECK(deliver(r, build(Id + 4, codec.data())) == 0);
        auto partial = build(Id + 5); CHECK(send(r, partial.packets[0], partial.id) == 0);
        validate([r tryTakeFrame], Id + 2, 1); CHECK(![r tryTakeFrame]);
        CHECK(deliver(r, build(Id + 6)) == 1);
        [r stop]; [r stop]; CHECK(![r tryTakeFrame]); CHECK(deliver(r, build(Id + 7)) == 0);
        r = nil;
    }
    CHECK(witness == nil); // Pool drained: coordinator deallocation must precede frame use.
    validate(retained, Id, 1); // Taken owner survives stop AND witnessed deallocation.
}
static void testTiming()
{
    for (unsigned fault = 0; fault < 5; ++fault) {
        Clock c; auto *r = create(c); const auto s = build();
        CHECK(send(r, s.packets[2]) == 0); CHECK(send(r, s.packets[3]) == 0);
        if (fault < 2) {
            c.script = {{100, 100, 100, fault == 0 ? uint64_t(100100) : uint64_t(99)}};
            c.count = 4; // Receiver's 3 samples, then post-wrapper sample.
        }
        CHECK(send(r, s.packets[1]) == (fault < 2 ? 0 : 1));
        CHECK(c.i == c.count);
        if (fault == 0) c.now = 100100; // Preserve monotonicity after scripted expiry.
        if (fault == 2) c.now = 100100; // Exact deadline at dequeue.
        if (fault == 3) c.now = 99; // Regression at dequeue must permanently stop.
        if (fault == 4) c.now = 100099;
        auto *frame = [r tryTakeFrame];
        if (fault == 4) validate(frame, Id, 1); else CHECK(!frame);
        c.count = 0; c.now = 100101;
        // Expiry permits a newer frame; clock regression is permanently fatal.
        CHECK(deliver(r, build(Id + 1), c.now) == ((fault == 1 || fault == 3) ? 0 : 1));
    }
    Clock c; auto *r = create(c); CHECK(deliver(r, build()) == 1);
    c.now = 99; const auto next = build(Id + 1);
    CHECK(send(r, next.packets[0], next.id) == 0); // Receiver proxy sees regression.
    c.now = 100; CHECK(![r tryTakeFrame]); CHECK(deliver(r, next) == 0);
}
static void testGenerations()
{
    Clock a, b; __strong MLPyrowaveFrame *old = nil; // Borrowed clocks outlive pool.
    __weak MLPyrowaveCoordinator *witness = nil;
    @autoreleasepool {
        auto *x = create(a, 11); auto *y = create(b, 22); witness = x;
        CHECK(deliver(x, build()) == 1); CHECK(deliver(y, build()) == 1);
        old = [x tryTakeFrame]; [x stop]; x = nil;
        validate([y tryTakeFrame], Id, 22);
        CHECK(deliver(y, build(Id + 1)) == 1); validate([y tryTakeFrame], Id + 1, 22);
    }
    CHECK(witness == nil); // Drain + weak nil, not just clearing x's strong reference.
    validate(old, Id, 11);
}
// Deterministic trylock contention only; runs in macOS CPU CI, never locally.
struct BlockingClock {
    dispatch_semaphore_t entered = dispatch_semaphore_create(0);
    dispatch_semaphore_t release = dispatch_semaphore_create(0);
    bool block = false;
};
static uint64_t blockingTick(void *p)
{
    auto &c = *static_cast<BlockingClock *>(p);
    if (c.block) {
        c.block = false;
        dispatch_semaphore_signal(c.entered);
        CHECK(dispatch_semaphore_wait(c.release, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    }
    return 100;
}
static void testContention()
{
    BlockingClock c;
    auto *r = [[MLPyrowaveCoordinator alloc] initWithConfiguration:config()
                                                           clock:blockingTick context:&c];
    CHECK(r); CHECK(deliver(r, build()) == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
    c.block = true; auto *context = &c; auto done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            LI_PYROWAVE_RECEIVED_PACKET invalid = {};
            CHECK([r submitPacket:&invalid] == LI_PYROWAVE_PACKET_IGNORED);
            dispatch_semaphore_signal(done);
        }
    });
    CHECK(dispatch_semaphore_wait(context->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    BOOL contention = NO;
    CHECK(![r tryTakeFrameWithContention:&contention] && contention);
    dispatch_semaphore_signal(c.release);
    CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
    validate([r tryTakeFrameWithContention:&contention], Id, 1); CHECK(!contention);
    contention = YES; CHECK(![r tryTakeFrameWithContention:&contention] && !contention);
    CHECK(![r tryTakeFrameWithContention:nullptr]); // Optional out parameter, ordinary empty.
    [r stop]; contention = YES;
    CHECK(![r tryTakeFrameWithContention:&contention] && !contention);
}
static void testConcurrency()
{
    auto *r = [[MLPyrowaveCoordinator alloc] initWithConfiguration:config(33)
                                                           clock:constantClock context:nullptr];
    CHECK(r); auto group = dispatch_group_create();
    auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_async(group, queue, ^{
        @autoreleasepool {
            for (uint32_t i = 0; i < 100; ++i) {
                const int result = deliver(r, build(Id + i)); CHECK(result == 0 || result == 1);
            }
        }
    });
    dispatch_group_async(group, queue, ^{
        @autoreleasepool {
            for (unsigned i = 0; i < 500; ++i) {
                auto *frame = [r tryTakeFrame];
                if (frame) {
                    const auto id = [frame validatedFrame]->codec->frameInfo().frameId;
                    CHECK(id >= Id && id < Id + 100); validate(frame, id, 33);
                }
            }
        }
    });
    dispatch_group_async(group, queue, ^{
        @autoreleasepool {
            [r stop];
            CHECK(deliver(r, build(Id + 200)) == 0); // Even while producer may run.
        }
    });
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    CHECK(![r tryTakeFrame]); CHECK(deliver(r, build(Id + 100)) == 0);
    // Scheduling-dependent stress; does NOT prove the busy trylock timing path.
}
int main()
{
    @autoreleasepool {
        testFactory(); testOwnershipAndSlot(); testTiming(); testGenerations(); testContention(); testConcurrency();
    }
    std::puts("coordinator CPU checks passed"); return 0;
}
