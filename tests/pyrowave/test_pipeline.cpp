// Synthetic fixed literal from test_packetizer.cpp's pinned CPU golden, NOT GPU output.
// Finite deterministic mutation smoke, not coverage-guided fuzzing or a fuzz proof.
#include "MLPyrowaveCodec.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)
#define R(name) LI_PYROWAVE_REASSEMBLY_##name
using namespace MLPyrowave;
using Owned = std::unique_ptr<const ValidatedCodecFrame>;
static const std::array<uint8_t, 24> Codec = {{0x7f, 0xc0, 0x1f, 0xd0, 1, 0, 0, 0,
    1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0}};
static const std::array<uint8_t, 10> Latency = {{1, 0, 0, 5, 0, 0, 0, 2, 0x12, 0x34}};
static constexpr uint32_t FrameId = 0x12345678, Timestamp = 0x87654321;
struct Packet { std::array<uint8_t, 256> bytes = {}; size_t length = 0; };
struct Shards { std::array<Packet, 4> packets; uint16_t count = 0; };
struct Copy {
    std::array<uint8_t, 256> bytes;
    size_t length = 99;
    MLPyrowaveFrameInfo info = {};
    LI_PYROWAVE_REASSEMBLY_RESULT status = R(NOT_READY);
};

static CodecSession session()
{
    CodecSession s; s.width = s.height = 128; return s;
}

// Same header semantics as test_reassembly.c; all bodies padded to B=16.
static LI_PYROWAVE_PACKET_HEADER header(uint32_t id, uint16_t i, uint16_t n,
                                       size_t codecSize, uint16_t metaSize)
{
    LI_PYROWAVE_PACKET_HEADER h = {};
    h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
    h.frameId = id; h.rtpTimestamp = Timestamp;
    h.packetKind = i == n ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
    h.dataBlockCount = n; h.parityBlockCount = 1; h.blockCount = n + 1; h.blockIndex = i;
    h.fecBlockPayloadSize = h.payloadLength = 16;
    h.codecPayloadLength = uint32_t(codecSize); h.metadataLength = metaSize;
    h.protectedPayloadLength = uint32_t(codecSize + metaSize);
    h.metadataFlags = metaSize ? LI_PYROWAVE_METADATA_FLAG_PROTECTED : 0;
    h.flags = metaSize ? LI_PYROWAVE_FLAG_METADATA_PRESENT : 0;
    h.fecScheme = LI_PYROWAVE_FEC_SCHEME_XOR; h.fecGroupIndex = 0;
    h.fecDataCount = uint8_t(n); h.fecParityCount = 1; h.fecShardIndex = uint8_t(i);
    if (i == n) h.flags |= LI_PYROWAVE_FLAG_FEC_PARITY;
    else {
        if (i == 0) h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
        if (i + 1 == n) h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
    }
    return h;
}

static Shards build(uint32_t id, const uint8_t *codec, size_t size,
                    const uint8_t *meta = Latency.data(), uint16_t metaSize = 10)
{
    std::array<uint8_t, 48> protectedBytes = {};
    CHECK(size + metaSize <= protectedBytes.size());
    if (metaSize) std::memcpy(protectedBytes.data(), meta, metaSize);
    std::memcpy(protectedBytes.data() + metaSize, codec, size);
    Shards s; s.count = uint16_t((size + metaSize + 15) / 16);
    CHECK(s.count > 0 && s.count <= 3);
    std::array<uint8_t, 16> parity = {};
    for (uint16_t i = 0; i <= s.count; ++i) {
        const uint8_t *body = i == s.count ? parity.data() : protectedBytes.data() + i * 16;
        auto h = header(id, i, s.count, size, metaSize);
        auto &p = s.packets[i];
        CHECK(LiPyrowaveBuildPacket(&h, body, 16, p.bytes.data(), p.bytes.size(), &p.length) ==
              LI_PYROWAVE_PACKET_OK);
        if (i < s.count)
            for (size_t j = 0; j < 16; ++j) parity[j] ^= body[j];
    }
    return s;
}

static LI_PYROWAVE_REASSEMBLY_RESULT push(MLPyrowaveReassembly *ctx,
                                         const Packet &p, uint64_t now)
{
    const auto before = p.bytes;
    const auto r = MLPyrowaveReassemblyPush(ctx, p.bytes.data(), p.length, now);
    CHECK(std::equal(before.begin(), before.end(), p.bytes.begin())); return r;
}

static void checkOwned(const ValidatedCodecFrame &v, const uint8_t *bytes, size_t size,
                       const MLPyrowaveFrameInfo &info)
{
    CHECK(v.payloadSize() == size && v.bytes().size() == size + 16);
    CHECK(std::equal(bytes, bytes + size, v.bytes().begin()));
    CHECK(std::all_of(v.bytes().begin() + size, v.bytes().end(), [](uint8_t b) { return b == 0; }));
    const auto &f = v.frameInfo(); const auto &s = v.session();
    CHECK(f.frameId == info.frameId && f.rtpTimestamp == info.rtpTimestamp);
    CHECK(f.firstReceiveUs == info.firstReceiveUs && f.deadlineUs == info.deadlineUs);
    CHECK(f.metadata.hasHostProcessingLatency == info.metadata.hasHostProcessingLatency);
    CHECK(f.metadata.hostProcessingLatency == info.metadata.hostProcessingLatency);
    CHECK(s.width == 128 && s.height == 128 && s.bitDepth == 8 && s.chroma == 0);
    CHECK(s.primaries == 0 && s.transfer == 0 && s.transform == 0 && s.range == 0 && s.siting == 0);
}

// The sole publication gate: never invoke the codec validator on failed Copy.
static Owned publish(MLPyrowaveReassembly *ctx, uint64_t now, Copy &c, unsigned &calls)
{
    c.bytes.fill(0xa5); c.length = 99;
    c.info = {99, 99, 99, 99, {true, 99}};
    c.status = MLPyrowaveReassemblyCopyFrame(ctx, now, c.bytes.data(), c.bytes.size(),
                                           &c.length, &c.info);
    if (c.status == R(COMPLETE)) {
        CHECK(c.length <= c.bytes.size()); ++calls;
        const auto before = c.bytes;
        auto v = ValidateCodecFrame(c.bytes.data(), c.length, session(), c.info);
        CHECK(c.bytes == before);
        if (v) checkOwned(*v, c.bytes.data(), c.length, c.info);
        CHECK(std::all_of(c.bytes.begin() + c.length, c.bytes.end(),
                          [](uint8_t b) { return b == 0xa5; }));
        return v;
    }
    CHECK(c.length == 0);
    const std::array<uint8_t, sizeof(c.info)> zero = {};
    CHECK(std::memcmp(&c.info, zero.data(), zero.size()) == 0);
    CHECK(std::all_of(c.bytes.begin(), c.bytes.end(), [](uint8_t b) { return b == 0xa5; }));
    return nullptr;
}

static void deliver(MLPyrowaveReassembly *ctx, const Shards &s, uint64_t now)
{
    for (uint16_t i = s.count; i > 0; --i)
        CHECK(push(ctx, s.packets[i - 1], now + s.count - i) ==
              (i == 1 ? R(COMPLETE) : R(ACCEPTED)));
}

static void testRecoveryOwnership()
{
    auto s = build(FrameId, Codec.data(), Codec.size()); CHECK(s.count == 3);
    auto *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx);
    std::unique_ptr<Copy> c(new Copy); unsigned calls = 0;
    CHECK(push(ctx, s.packets[2], 100) == R(ACCEPTED));
    CHECK(!publish(ctx, 100, *c, calls) && c->status == R(NOT_READY) && calls == 0);
    CHECK(push(ctx, s.packets[3], 101) == R(ACCEPTED)); // Two missing DATA + parity.
    CHECK(!publish(ctx, 101, *c, calls) && c->status == R(NOT_READY) && calls == 0);
    CHECK(push(ctx, s.packets[1], 102) == R(COMPLETE)); // Recover DATA0: TLV + codec prefix.
    auto v = publish(ctx, 103, *c, calls); CHECK(v && calls == 1);
    CHECK(c->length == Codec.size() && std::equal(Codec.begin(), Codec.end(), c->bytes.begin()));
    CHECK(c->info.frameId == FrameId && c->info.rtpTimestamp == Timestamp);
    CHECK(c->info.firstReceiveUs == 100 && c->info.deadlineUs == 100100);
    CHECK(c->info.metadata.hasHostProcessingLatency && c->info.metadata.hostProcessingLatency == 0x1234);
    const auto info = c->info;
    MLPyrowaveReassemblyDestroy(ctx);
    c->bytes.fill(0); c->info = {}; c.reset();
    for (auto &p : s.packets) p.bytes.fill(0);
    checkOwned(*v, Codec.data(), Codec.size(), info);
    CHECK(v->wordOffsets().size() == 51 && v->wordOffsets()[0] == 2);
    CHECK(std::all_of(v->wordOffsets().begin() + 1, v->wordOffsets().end(),
                      [](uint32_t o) { return o == UINT32_MAX; }));
}

static void testInvalidCodec()
{
    for (unsigned fault = 0; fault < 5; ++fault) {
        std::array<uint8_t, 32> bad = {}; std::copy(Codec.begin(), Codec.end(), bad.begin());
        size_t size = Codec.size();
        if (fault == 0) bad[11] ^= 0x10; // Coefficient sequence conflict.
        if (fault == 1) bad[7] |= 0x10; // Unsupported sequence code.
        if (fault == 2) bad[7] |= 1; // Color contract conflict.
        if (fault == 3) bad[4] = 2; // Declared unique count mismatch.
        if (fault == 4) { // Duplicate index0, zero words: must reject before duplicate fast path.
            std::copy(Codec.begin() + 8, Codec.begin() + 16, bad.begin() + 24);
            bad[26] = 0; size = 32;
        }
        auto *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx);
        deliver(ctx, build(FrameId, bad.data(), size), 100);
        Copy c; unsigned calls = 0;
        CHECK(!publish(ctx, 104, c, calls) && c.status == R(COMPLETE) && calls == 1);
        CHECK(c.length == size && std::equal(bad.begin(), bad.begin() + size, c.bytes.begin()));
        MLPyrowaveReassemblyDestroy(ctx);
    }
}

static void testMetadataRecoveryAndExpiry()
{
    auto *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx);
    auto bad = Latency; bad[7] = 3;
    deliver(ctx, build(FrameId, Codec.data(), Codec.size(), bad.data()), 100);
    Copy c; unsigned calls = 0;
    CHECK(!publish(ctx, 104, c, calls) && c.status == R(INVALID_PACKET) && calls == 0);
    deliver(ctx, build(FrameId + 1, Codec.data(), Codec.size()), 200);
    auto v = publish(ctx, 204, c, calls); CHECK(v && calls == 1);
    CHECK(v->frameInfo().metadata.hasHostProcessingLatency);
    deliver(ctx, build(FrameId + 2, Codec.data(), Codec.size(), nullptr, 0), 300);
    v = publish(ctx, 304, c, calls); CHECK(v && calls == 2);
    CHECK(v->frameInfo().frameId == FrameId + 2);
    CHECK(!v->frameInfo().metadata.hasHostProcessingLatency &&
          v->frameInfo().metadata.hostProcessingLatency == 0);
    v.reset(); deliver(ctx, build(FrameId + 3, Codec.data(), Codec.size()), 400);
    CHECK(!publish(ctx, 100400, c, calls) && c.status == R(EXPIRED) && calls == 2);
    CHECK(!publish(ctx, 100401, c, calls) && c.status == R(NOT_READY) && calls == 2);
    MLPyrowaveReassemblyDestroy(ctx);
}

static uint32_t next(uint32_t &state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}

static void mutate(uint8_t *bytes, size_t &size, size_t capacity, uint32_t &rng, unsigned mode)
{
    CHECK(size > 0 && size <= capacity);
    if (mode % 4 == 0) {
        const size_t offset = next(rng) % size;
        bytes[offset] ^= uint8_t(1u << (next(rng) % 8));
    }
    if (mode % 4 == 1) size = next(rng) % size;
    if (mode % 4 == 2) {
        const size_t end = size + 1 + next(rng) % (capacity - size);
        while (size < end) bytes[size++] = uint8_t(next(rng));
    }
    // Fourth case retains the valid seed, ensuring accepted paths are exercised.
}

static void testValidatorSmoke(uint32_t &rng)
{
    MLPyrowaveFrameInfo info = {}; info.frameId = FrameId; info.rtpTimestamp = Timestamp;
    info.firstReceiveUs = 100; info.deadlineUs = 100100; info.metadata = {true, 0x1234};
    for (unsigned i = 0; i < 1024; ++i) {
        std::array<uint8_t, 128> bytes = {}; std::copy(Codec.begin(), Codec.end(), bytes.begin());
        size_t size = Codec.size(); mutate(bytes.data(), size, bytes.size(), rng, i);
        const auto before = bytes;
        auto v = ValidateCodecFrame(bytes.data(), size, session(), info);
        CHECK(bytes == before);
        if (v) {
            bytes.fill(0); checkOwned(*v, before.data(), size, info);
        } else CHECK(v == nullptr);
        bytes.fill(0); std::copy(Latency.begin(), Latency.end(), bytes.begin());
        size = Latency.size(); mutate(bytes.data(), size, bytes.size(), rng, i);
        const auto metadataBefore = bytes;
        MLPyrowaveMetadata out = {true, 99};
        if (!MLPyrowaveValidateMetadata(bytes.data(), size, LI_PYROWAVE_METADATA_FLAG_PROTECTED, &out))
            CHECK(!out.hasHostProcessingLatency && out.hostProcessingLatency == 0);
        CHECK(std::equal(metadataBefore.begin(), metadataBefore.end(), bytes.begin()));
    }
}

static void testWireSmoke(uint32_t &rng)
{
    const auto seeds = build(FrameId, Codec.data(), Codec.size());
    for (unsigned i = 0; i < 256; ++i) {
        auto *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx);
        const uint16_t index = uint16_t(next(rng) % 4);
        auto changed = seeds.packets[index];
        mutate(changed.bytes.data(), changed.length, changed.bytes.size(), rng, i);
        (void)push(ctx, changed, 100);
        for (uint16_t j = 0; j < 4; ++j)
            if (j != index) (void)push(ctx, seeds.packets[j], 101 + j);
        Copy c; unsigned calls = 0;
        auto v = publish(ctx, 106, c, calls);
        const auto bytes = c.bytes; const auto info = c.info; const size_t size = c.length;
        CHECK(calls == (c.status == R(COMPLETE) ? 1u : 0u));
        MLPyrowaveReassemblyDestroy(ctx); changed.bytes.fill(0); c.bytes.fill(0); c.info = {};
        if (v) checkOwned(*v, bytes.data(), size, info);
        else CHECK(v == nullptr); // Mutations need not all reject.
    }
}

int main()
{
    testRecoveryOwnership(); testInvalidCodec(); testMetadataRecoveryAndExpiry();
    uint32_t rng = 0x6d2b79f5; testValidatorSmoke(rng); testWireSmoke(rng);
    std::puts("pipeline composition and finite mutation smoke passed"); return 0;
}
