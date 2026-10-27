// Synthetic CPU composition, not encrypted network/GPU/live app wiring.
#import "MLPyrowaveCoordinator.h"
#import "MLPyrowavePacketBridge.h"
#include "MLPyrowaveReceiver.hpp"
#include "common_coordinator_transport.h"
#include <algorithm>
#include <array>
#include <climits>
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
struct Clock { uint64_t now = 100; };
static uint64_t tick(void *p) { return static_cast<Clock *>(p)->now; }
static uint64_t liveTick(void *) { return LiGetMicroseconds(); }
@interface Capture : NSObject <MLPyrowavePacketSink>
@property(nonatomic, strong) MLPyrowaveCoordinator *coordinator;
@property(nonatomic) unsigned calls;
@property(nonatomic) int last;
@end
@implementation Capture
- (int)submitPyrowavePacket:(const LI_PYROWAVE_RECEIVED_PACKET *)packet
{
    self.calls++;
    self.last = [self.coordinator submitPacket:packet]; // Actual result, no bypass.
    return self.last;
}
@end
struct Fixture {
    __strong Capture *capture;
    CommonTransport *transport;
    Fixture(Clock *clock, uint64_t generation) : capture([Capture new]) {
        capture.last = -1;
        const MLPyrowaveConfiguration config = {128, 128, 256, 0, 0, generation};
        capture.coordinator = [[MLPyrowaveCoordinator alloc] initWithConfiguration:config
                                      clock:clock ? tick : liveTick context:clock];
        CHECK(capture.coordinator);
        const auto callbacks = MLPyrowaveCallbacksForSink(capture);
        transport = transportCreate(&callbacks); CHECK(transport);
        CHECK(!transportFullFrame(transport));
    }
    ~Fixture() {
        [capture.coordinator stop];
        transportDestroy(transport); // Clears TLS, cleans real queue, frees context.
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};
static void testPresentationConfiguration()
{
    const MLPyrowaveConfiguration valid = {128, 128, 256, 0, 0, 1};
    CHECK(MLPyrowaveValidatePresentationConfiguration(valid, 60));
    auto bad = valid; bad.generation = 0;
    CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
    for (uint32_t packet : {0u, 128u, 65537u, UINT32_MAX}) {
        bad = valid; bad.maxPacketSize = packet;
        CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
    }
    for (uint32_t dimension : {0u, 1u, 3u, 16385u, 16386u, UINT32_MAX}) {
        bad = valid; bad.width = dimension;
        CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
        bad = valid; bad.height = dimension;
        CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
    }
    for (uint8_t value : {2, 255}) {
        bad = valid; bad.range = value;
        CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
        bad = valid; bad.siting = value;
        CHECK(!MLPyrowaveValidatePresentationConfiguration(bad, 60));
    }
    for (int fps : {0, -1, INT_MIN})
        CHECK(!MLPyrowaveValidatePresentationConfiguration(valid, fps));
    for (uint32_t width : {2u, 16384u}) for (uint32_t height : {2u, 16384u})
        for (uint32_t packet : {129u, 65536u}) for (uint8_t range : {0, 1})
            for (uint8_t siting : {0, 1}) for (int fps : {1, INT_MAX}) {
                const MLPyrowaveConfiguration edge = {width, height, packet, range, siting, UINT64_MAX};
                CHECK(MLPyrowaveValidatePresentationConfiguration(edge, fps));
            } // CPU shape limits only, NOT GPU availability or footprint assertions.
}
static void testAdapterDefenses()
{
    const auto disabled = MLPyrowaveCallbacksForSink(nil);
    CHECK(disabled.version == 0 && !disabled.context && !disabled.submit);
    Clock clock; Fixture f(&clock, 1);
    const auto callbacks = MLPyrowaveCallbacksForSink(f.capture);
    CHECK(callbacks.version == LI_PYROWAVE_PACKET_CALLBACK_VERSION2);
    CHECK(callbacks.context == (__bridge void *)f.capture && callbacks.submit);
    const LI_PYROWAVE_RECEIVED_PACKET empty = {};
    CHECK(callbacks.submit(nullptr, &empty) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(callbacks.submit(callbacks.context, nullptr) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(f.capture.calls == 0);
    CHECK(callbacks.submit(callbacks.context, &empty) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(f.capture.calls == 1 && f.capture.last == LI_PYROWAVE_PACKET_IGNORED);
    // Existing composition cases below prove exact VALIDATED_FRAME forwarding.
}
static int send(Fixture &f, const Packet &p, uint32_t id, uint64_t receive = 100)
{
    const unsigned before = f.capture.calls;
    transportDirect(f.transport, p.bytes.data(), p.length, id, Outer, receive, 9000);
    CHECK(f.capture.calls == before + 1);
    return f.capture.last;
}
static void deliver(Fixture &f, const Shards &s, int expected)
{
    CHECK(send(f, s.packets[2], s.id) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(!transportFullFrame(f.transport));
    CHECK(send(f, s.packets[3], s.id) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(!transportFullFrame(f.transport)); // Still missing DATA[1].
    CHECK(send(f, s.packets[1], s.id) == expected); // XOR recovers DATA[0].
    CHECK(transportFullFrame(f.transport) == (expected == LI_PYROWAVE_PACKET_VALIDATED_FRAME));
}
static void validate(MLPyrowaveFrame *frame, uint32_t id, uint64_t generation,
                     uint64_t receive, uint64_t presentation)
{
    CHECK(frame && frame.generation == generation);
    const auto *v = [frame validatedFrame]; CHECK(v && v->codec);
    const auto &info = v->codec->frameInfo();
    CHECK(info.frameId == id && info.rtpTimestamp == Inner && v->outerRtpTimestamp == Outer);
    CHECK(info.firstReceiveUs == receive && info.deadlineUs == receive + 100000);
    CHECK(v->presentationTimeUs == presentation); // Never subtract RTP from local time.
    CHECK(info.metadata.hasHostProcessingLatency && info.metadata.hostProcessingLatency == 0x1234);
    CHECK(v->codec->payloadSize() == Codec.size() && v->codec->bytes().size() == 40);
    CHECK(std::equal(Codec.begin(), Codec.end(), v->codec->bytes().begin()));
    CHECK(std::all_of(v->codec->bytes().begin() + 24, v->codec->bytes().end(),
                      [](uint8_t b) { return b == 0; }));
    CHECK(v->codec->offsets().size() == 51 && v->codec->offsets()[0] == 8);
}
static void testDirectIsolationAndOwnership()
{
    Clock clock; __strong MLPyrowaveFrame *owned = nil;
    __weak MLPyrowaveCoordinator *witness = nil;
    @autoreleasepool {
        Fixture a(&clock, 11), b(&clock, 22); witness = a.capture.coordinator;
        auto shards = build();
        transportTls(b.transport); // Explicit depacketizer context must beat TLS.
        for (unsigned version : {0u, 1u, 2u}) {
            transportGates(a.transport, version, version != 2);
            transportDirect(a.transport, shards.packets[2].bytes.data(), shards.packets[2].length,
                            Id, Outer, 100, 9000);
        }
        CHECK(a.capture.calls == 0 && b.capture.calls == 0);
        CHECK(!transportFullFrame(a.transport) && !transportFullFrame(b.transport));
        transportGates(a.transport, LI_PYROWAVE_PACKET_CALLBACK_VERSION2, true);
        auto malformed = shards.packets[0]; malformed.bytes[0] ^= 1;
        CHECK(send(a, malformed, Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(!transportFullFrame(a.transport) && ![a.capture.coordinator tryTakeFrame]);
        CHECK(send(a, shards.packets[2], Id) == LI_PYROWAVE_PACKET_IGNORED);
        transportTls(a.transport);
        CHECK(send(b, shards.packets[2], Id) == LI_PYROWAVE_PACKET_IGNORED);
        transportTls(b.transport);
        CHECK(send(a, shards.packets[3], Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(!transportFullFrame(a.transport) && !transportFullFrame(b.transport));
        CHECK(![a.capture.coordinator tryTakeFrame] && ![b.capture.coordinator tryTakeFrame]);
        CHECK(send(a, shards.packets[1], Id) == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
        CHECK(transportFullFrame(a.transport) && !transportFullFrame(b.transport));
        CHECK(a.capture.calls == 4 && b.capture.calls == 1);
        owned = [a.capture.coordinator tryTakeFrame]; validate(owned, Id, 11, 100, 9000);
        CHECK(![a.capture.coordinator tryTakeFrame]);
        // Observation-only reset detects a NEW ack, not a stale latched success.
        transportResetObservation(a.transport);
        transportTls(a.transport);
        CHECK(send(b, shards.packets[3], Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(!transportFullFrame(a.transport) && !transportFullFrame(b.transport));
        CHECK(send(b, shards.packets[1], Id) == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
        CHECK(!transportFullFrame(a.transport) && transportFullFrame(b.transport));
        CHECK(a.capture.calls == 4 && b.capture.calls == 3);
        validate([b.capture.coordinator tryTakeFrame], Id, 22, 100, 9000);
        transportTls(b.transport);
        deliver(a, shards, LI_PYROWAVE_PACKET_IGNORED); // Same-ID replay after take.
        CHECK(![a.capture.coordinator tryTakeFrame]);
        [a.capture.coordinator stop];
        deliver(a, build(Id + 1), LI_PYROWAVE_PACKET_IGNORED);
        CHECK(![a.capture.coordinator tryTakeFrame]);
        for (auto &p : shards.packets) p.bytes.fill(0);
        // Each real Common call already freed its malloc envelope on return.
        // Sanitized use after context/coordinator teardown checks independent ownership.
    }
    CHECK(witness == nil);
    validate(owned, Id, 11, 100, 9000);
}
static void testBadCodecAndExpiry()
{
    Clock clock;
    {
        Fixture f(&clock, 31);
        auto badCodec = Codec; badCodec[4] = 2;
        deliver(f, build(Id, badCodec.data()), LI_PYROWAVE_PACKET_IGNORED);
        CHECK(![f.capture.coordinator tryTakeFrame]); // Reassembly completed, codec invalid.
    }
    {
        Fixture f(&clock, 32); const auto s = build();
        CHECK(send(f, s.packets[2], Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(send(f, s.packets[3], Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(!transportFullFrame(f.transport));
        clock.now = 100100; // Exact first-receive +100000, not deadline+1.
        CHECK(send(f, s.packets[1], Id) == LI_PYROWAVE_PACKET_IGNORED);
        CHECK(!transportFullFrame(f.transport) && ![f.capture.coordinator tryTakeFrame]);
    }
}
static void testRealRtpIngress()
{
    Fixture f(nullptr, 41), other(nullptr, 42);
    transportTls(other.transport); // Also verify isolation through the real queue.
    const auto s = build(0xfedcba98);
    const uint64_t before = LiGetMicroseconds();
    const uint64_t first = transportRtp(f.transport, s.packets[2].bytes.data(),
                                      s.packets[2].length, s.id, Outer);
    CHECK(first >= before && first <= LiGetMicroseconds());
    CHECK(f.capture.calls == 1 && f.capture.last == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(!transportFullFrame(f.transport) && ![f.capture.coordinator tryTakeFrame]);
    const uint64_t second = transportRtp(f.transport, s.packets[3].bytes.data(),
                                       s.packets[3].length, s.id, Outer);
    CHECK(second >= first && second <= LiGetMicroseconds());
    CHECK(f.capture.calls == 2 && f.capture.last == LI_PYROWAVE_PACKET_IGNORED);
    CHECK(!transportFullFrame(f.transport));
    const uint64_t final = transportRtp(f.transport, s.packets[1].bytes.data(),
                                      s.packets[1].length, s.id, Outer);
    CHECK(final >= second && final <= LiGetMicroseconds());
    CHECK(f.capture.calls == 3 && f.capture.last == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
    CHECK(transportFullFrame(f.transport) && !transportFullFrame(other.transport));
    CHECK(other.capture.calls == 0 && ![other.capture.coordinator tryTakeFrame]);
    auto *frame = [f.capture.coordinator tryTakeFrame];
    validate(frame, s.id, 41, first, (uint64_t(Outer) * 1000) / 90);
    CHECK(![f.capture.coordinator tryTakeFrame]);
}
int main()
{
    @autoreleasepool {
        testPresentationConfiguration(); testAdapterDefenses();
        testDirectIsolationAndOwnership(); testBadCodecAndExpiry(); testRealRtpIngress();
    }
    std::puts("actual Common -> coordinator -> watchdog CPU checks passed");
    return 0; // This non-GPU target has no skip/77 path.
}
