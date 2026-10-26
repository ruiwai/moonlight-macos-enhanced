// Synthetic CPU literals from test_pipeline.cpp, NOT GPU encoder fixtures.
// Raw-descriptor production API only; no actual Common queue/app call path.
#include "MLPyrowaveReceiver.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)
using namespace MLPyrowave;
using Owned = std::unique_ptr<const ReceivedFrame>;
static const std::array<uint8_t, 24> Codec = {{0x7f, 0xc0, 0x1f, 0xd0, 1, 0, 0, 0,
    1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0}};
static const std::array<uint8_t, 10> Latency = {{1, 0, 0, 5, 0, 0, 0, 2, 0x12, 0x34}};
static constexpr uint32_t Id = 0x12345678, Inner = 0x87654321, Outer = 0xabcdef01;
struct Packet { std::array<uint8_t, 256> bytes = {}; size_t length = 0; };
struct Shards { std::array<Packet, 4> packets; uint16_t count = 0; uint32_t id = 0; };
struct Clock { uint64_t now = 100; std::array<uint64_t, 3> script = {}; size_t count = 0, i = 0; };
static void program(Clock &c, std::array<uint64_t, 3> values, size_t count)
{
    CHECK(count <= c.script.size());
    std::copy(values.begin(), values.end(), c.script.begin()); c.count = count; c.i = 0;
}
static uint64_t tick(void *p)
{
    auto &c = *static_cast<Clock *>(p); return c.i < c.count ? c.script[c.i++] : c.now;
}
static CodecSession session()
{
    CodecSession s; s.width = s.height = 128; return s;
}
static std::unique_ptr<Receiver> create(Clock &c)
{
    auto r = Receiver::Create(session(), 256, tick, &c); CHECK(r); return r;
}
// Real Common serializer, B=16 padded DATA and one XOR parity, as in pipeline.
static Shards build(uint32_t id = Id, const uint8_t *codec = Codec.data(),
                    const uint8_t *meta = Latency.data(), uint16_t metaSize = 10)
{
    std::array<uint8_t, 48> bytes = {}; CHECK(Codec.size() + metaSize <= bytes.size());
    if (metaSize) std::memcpy(bytes.data(), meta, metaSize);
    std::memcpy(bytes.data() + metaSize, codec, Codec.size());
    Shards s; s.id = id; s.count = uint16_t((Codec.size() + metaSize + 15) / 16);
    CHECK(s.count > 0 && s.count <= 3); std::array<uint8_t, 16> parity = {};
    for (uint16_t i = 0; i <= s.count; ++i) {
        LI_PYROWAVE_PACKET_HEADER h = {};
        h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
        h.frameId = id; h.rtpTimestamp = Inner;
        h.packetKind = i == s.count ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
        h.dataBlockCount = s.count; h.parityBlockCount = 1; h.blockCount = s.count + 1;
        h.blockIndex = i; h.fecBlockPayloadSize = h.payloadLength = 16;
        h.codecPayloadLength = uint32_t(Codec.size()); h.metadataLength = metaSize;
        h.protectedPayloadLength = uint32_t(Codec.size() + metaSize);
        h.metadataFlags = metaSize ? LI_PYROWAVE_METADATA_FLAG_PROTECTED : 0;
        h.flags = metaSize ? LI_PYROWAVE_FLAG_METADATA_PRESENT : 0;
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
static Owned send(Receiver &r, const Packet &p, uint32_t id = Id, uint64_t arrival = 100,
                  uint32_t outer = Outer, uint64_t presentation = 9000)
{
    const auto before = p.bytes;
    auto v = r.Receive({p.bytes.data(), p.length, id, outer, arrival, presentation});
    CHECK(std::equal(before.begin(), before.end(), p.bytes.begin())); return v;
}
static Owned deliver(Receiver &r, const Shards &s, uint64_t arrival = 100)
{
    for (uint16_t i = s.count; i > 1; --i)
        CHECK(!send(r, s.packets[i - 1], s.id, arrival));
    return send(r, s.packets[0], s.id, arrival);
}
static void testOwnershipAndTiming()
{
    Clock c; auto r = create(c); auto s = build();
    CHECK(!send(*r, s.packets[2]));
    CHECK(!send(*r, s.packets[2], Id, 99, Outer, 111)); // Duplicate cannot change timing.
    const auto stale = build(Id - 1);
    CHECK(!send(*r, stale.packets[0], Id - 1, 0, Outer - 1, 222));
    CHECK(!send(*r, s.packets[3], Id, 99, Outer, 333));
    auto v = send(*r, s.packets[1], Id, 100, Outer, 444); CHECK(v && v->codec);
    CHECK(!deliver(*r, s)); // Copy already retired the full ID.
    r.reset(); for (auto &p : s.packets) p.bytes.fill(0);
    const auto &f = v->codec->frameInfo();
    CHECK(f.frameId == Id && f.rtpTimestamp == Inner && v->outerRtpTimestamp == Outer);
    CHECK(v->presentationTimeUs == 9000 && f.firstReceiveUs == 100 && f.deadlineUs == 100100);
    CHECK(f.metadata.hasHostProcessingLatency && f.metadata.hostProcessingLatency == 0x1234);
    CHECK(v->codec->payloadSize() == 24 && v->codec->bytes().size() == 40);
    CHECK(std::equal(Codec.begin(), Codec.end(), v->codec->bytes().begin()));
    CHECK(std::all_of(v->codec->bytes().begin() + 24, v->codec->bytes().end(),
                      [](uint8_t b) { return b == 0; }));
    CHECK(v->codec->offsets().size() == 51 && v->codec->offsets()[0] == 8);
}
static void testPoisonAndReplay()
{
    for (unsigned fault = 0; fault < 6; ++fault) {
        Clock c; auto r = create(c); const auto s = build(); CHECK(!send(*r, s.packets[2]));
        auto p = s.packets[1]; auto id = Id; auto stamp = Outer;
        if (fault == 0) id ^= 0x10000; // Same low 16 bits is NOT full outer ID equality.
        if (fault == 1) ++stamp;
        if (fault == 2) p.bytes[0] ^= 1;
        if (fault == 3) p.length = 257; // Reject before any read beyond the array.
        if (fault == 4) p.length = 63;
        if (fault == 5) CHECK(!r->Receive({nullptr, 80, Id, Outer, 100, 0}));
        else CHECK(!send(*r, p, id, 100, stamp));
        CHECK(!deliver(*r, s)); CHECK(deliver(*r, build(Id + 1)));
    }
}
static void testDeadlinesAndClock()
{
    for (unsigned fault = 0; fault < 10; ++fault) {
        Clock c; auto r = create(c); const auto s = build();
        CHECK(!send(*r, s.packets[2])); CHECK(!send(*r, s.packets[1]));
        uint64_t arrival = 100;
        if (fault == 0) c.now = 100100; // Incomplete, exact local deadline, old arrival.
        if (fault == 1) c.now = 100101;
        if (fault == 2) { program(c, {{100, 100, 100100}}, 3); c.now = 100100; }
        if (fault == 3) { program(c, {{100, 100100, 0}}, 2); c.now = 100100; }
        if (fault == 4) c.now = 99;
        if (fault == 5) arrival = 101; // Future receive time.
        if (fault == 6) program(c, {{100, 99, 0}}, 2);
        if (fault == 7) program(c, {{100, 100, 99}}, 3);
        if (fault == 8) { c.now = UINT64_MAX; arrival = UINT64_MAX - 50; }
        if (fault == 9) c.now = 100099; // One microsecond before expiry succeeds.
        auto v = send(*r, s.packets[0], Id, arrival); CHECK(bool(v) == (fault == 9));
        CHECK(c.i == c.count); // Fresh checks consumed the complete scripted clock.
        c.count = 0; c.now = std::max<uint64_t>(c.now, 100101);
        CHECK(!deliver(*r, s, c.now)); // Failure/Copy must not reopen same ID.
        if (fault != 8) CHECK(deliver(*r, build(Id + 1), c.now));
    }
    Clock c; c.now = 100100; auto r = create(c);
    CHECK(!deliver(*r, build())); // Late even before the first validation starts.
    c.now = UINT64_MAX; CHECK(!deliver(*r, build(Id + 1), UINT64_MAX - 50));
}
static void testInvalidAndMetadata()
{
    for (unsigned fault = 0; fault < 3; ++fault) {
        Clock c; auto r = create(c); auto codec = Codec; auto meta = Latency;
        if (fault == 0) codec[11] ^= 0x10; // Complete transport is not a valid codec.
        if (fault == 1) codec[4] = 2;
        if (fault == 2) meta[7] = 3; // Malformed protected TLV.
        CHECK(!deliver(*r, build(Id, codec.data(), meta.data())));
        CHECK(!deliver(*r, build()));
        auto v = deliver(*r, build(Id + 1)); CHECK(v);
        CHECK(v->codec->frameInfo().metadata.hasHostProcessingLatency);
        v = deliver(*r, build(Id + 2, Codec.data(), nullptr, 0)); CHECK(v);
        CHECK(!v->codec->frameInfo().metadata.hasHostProcessingLatency &&
              v->codec->frameInfo().metadata.hostProcessingLatency == 0);
    }
}
static void testFactoryStopAndIsolation()
{
    Clock a, b; auto x = create(a); auto y = create(b); const auto s = build();
    a.now = 200; // Independent clock history must not reject y's time=100.
    CHECK(!send(*x, s.packets[2])); CHECK(!send(*y, s.packets[1]));
    x->Stop(); x->Stop(); CHECK(!deliver(*x, s)); CHECK(!deliver(*x, build(Id + 1)));
    CHECK(!send(*y, s.packets[2])); auto v = send(*y, s.packets[0]); CHECK(v);
    CHECK(v->codec->frameInfo().firstReceiveUs == 100);
    b.now = 99; CHECK(!deliver(*y, build(Id + 1))); // Regression after completed Copy.
    b.now = 100; CHECK(deliver(*y, build(Id + 1)));
    CHECK(!Receiver::Create(session(), 256, nullptr, &a));
    for (uint32_t limit : {0u, 128u, 65537u}) CHECK(!Receiver::Create(session(), limit, tick, &a));
    CHECK(Receiver::Create(session(), 129, tick, &a));
    CHECK(Receiver::Create(session(), 65536, tick, &a));
    auto bad = session(); bad.width = 127; CHECK(!Receiver::Create(bad, 256, tick, &a));
    bad = session(); bad.bitDepth = 10; CHECK(!Receiver::Create(bad, 256, tick, &a));
}
static void testBoundedUntrustedInput()
{
    // Finite bounded wire mutation smoke, not a fuzz proof; buffers stay immutable.
    const auto seeds = build();
    for (size_t i = 0; i < 128; ++i) {
        Clock c; auto r = create(c); auto p = seeds.packets[i % 4];
        if (i % 2) p.length = i % p.length;
        else p.bytes[i % p.length] ^= uint8_t(1u << (i % 8));
        (void)send(*r, p);
        CHECK(deliver(*r, build(Id + 1))); // New valid identity recovers independently.
    }
}
int main()
{
    testOwnershipAndTiming(); testPoisonAndReplay(); testDeadlinesAndClock();
    testInvalidAndMetadata(); testFactoryStopAndIsolation(); testBoundedUntrustedInput();
    std::puts("raw-descriptor receiver CPU checks passed"); return 0;
}
