// Copyright (c) 2025 Hans-Kristian Arntzen
// Copyright (c) 2026 AlkaidLab contributors
// SPDX-License-Identifier: MIT AND GPL-3.0-only
// AlkaidLab/pyrowave cc91cbf6363721d19d135d05ca8e3d8665ea6afd:
// metal/pyrowave_bitstream.{hpp,cpp}: BlockLayout::init,
// compute_num_packets, packetize. Hand-authored coefficients, NOT GPU output.
// Literal snapshots are checked only when this test executes in macOS CI.
// Never call BitstreamParser; packet boundaries are not PYRF frames.
#include "MLPyrowaveCodec.hpp"
#include "pyrowave_bitstream.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); } } while (0)
using Bytes = std::vector<uint8_t>;
using Records = std::vector<std::pair<uint32_t, Bytes>>;
using namespace MLPyrowave;
using namespace PyroWave;

static const Bytes Zero = {0x7f, 0xc0, 0x1f, 0xd0, 0, 0, 0, 0};
static const Bytes Sparse = {0x7f, 0xc0, 0x1f, 0xd0, 1, 0, 0, 0,
    1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0};
static const Bytes Coefficient = {1, 0, 4, 0x50, 0, 0, 0, 0,
    1, 0, 0, 1, 0, 0, 0, 0};

static void put32(Bytes &b, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b[offset + i] = uint8_t(value >> (8 * i));
}

static CodecSession session(const BlockLayout &layout)
{
    CodecSession s;
    s.width = uint32_t(layout.width); s.height = uint32_t(layout.height);
    return s; // Default SDR709/8-bit/420, full range, center siting.
}

static MLPyrowaveFrameInfo envelope()
{
    MLPyrowaveFrameInfo info = {};
    info.frameId = 0xabcdef12; // Full identity, deliberately NOT short sequence5.
    info.rtpTimestamp = 0x12345678;
    info.firstReceiveUs = 91; info.deadlineUs = 100091;
    info.metadata.hasHostProcessingLatency = true;
    info.metadata.hostProcessingLatency = 1234;
    return info;
}

static Bytes serialize(const BlockLayout &layout, const Records &records,
                       size_t boundary = 128, size_t expectedPackets = 0)
{
    std::vector<BitstreamPacket> meta(size_t(layout.block_count_32x32), BitstreamPacket{});
    // packetize reads meta[0] even for zero count. Aligned dummy8 has sequence5.
    std::vector<uint32_t> source(2, 0);
    const Bytes dummy = {0, 0, 0, 0x50, 0, 0, 0, 0};
    std::memcpy(source.data(), dummy.data(), 8);
    size_t total = 8;
    for (const auto &r : records) {
        CHECK(r.first < meta.size() && meta[r.first].num_words == 0);
        CHECK(r.second.size() >= 8 && r.second.size() % 4 == 0);
        const size_t offset = source.size(); // Metadata offsets are WORDS.
        meta[r.first] = {uint32_t(offset), uint32_t(r.second.size() / 4)};
        source.resize(offset + r.second.size() / 4);
        std::memcpy(source.data() + offset, r.second.data(), r.second.size());
        total += r.second.size();
    }
    const size_t capacity = compute_num_packets(layout, meta.data(), boundary, 0);
    CHECK(capacity > 0 && (!expectedPackets || capacity == expectedPackets));
    std::vector<Packet> packets(capacity + 1, Packet{SIZE_MAX, SIZE_MAX});
    Bytes output(total + 16, 0xa5), joined;
    const size_t count = packetize(layout, packets.data(), boundary, output.data(),
                                  total, meta.data(), source.data(), 0);
    CHECK(count == capacity && packets[count].offset == SIZE_MAX);
    CHECK(packets[count].size == SIZE_MAX);
    size_t consumed = 0;
    for (size_t i = 0; i < count; ++i) {
        CHECK(packets[i].offset == consumed && packets[i].size > 0);
        CHECK(packets[i].size <= boundary && packets[i].size <= total - consumed);
        joined.insert(joined.end(), output.begin() + consumed,
                      output.begin() + consumed + packets[i].size);
        consumed += packets[i].size;
    }
    CHECK(consumed == total && joined.size() == total);
    CHECK(std::equal(joined.begin(), joined.end(), output.begin()));
    CHECK(std::all_of(output.begin() + total, output.end(), [](uint8_t b) { return b == 0xa5; }));
    return joined;
}

static Bytes record(uint32_t index, uint16_t mask)
{
    unsigned n = 0;
    for (unsigned bit = 0; bit < 16; ++bit) n += (mask >> bit) & 1;
    CHECK(n <= 16);
    // N controls LE16, N q bytes, N plane bytes, pooled ceil(N/8) signs.
    const size_t size = (8 + 3 * n + n + (n + 7) / 8 + 3) & ~size_t(3);
    Bytes b(size, 0);
    put32(b, 0, mask | (uint32_t(size / 4) << 16) | (5u << 28));
    put32(b, 4, index << 8); // quant0, explicit LE header, not bitfield structs.
    for (unsigned i = 0; i < n; ++i) {
        b[8 + 2 * i] = 1; // control1, q0: exactly one plane for each active8x8.
        b[8 + 3 * n + i] = 1;
    }
    std::fill(b.begin() + 8 + 4 * n, b.end(), 0xa5); // Signs + unconstrained pad.
    if (mask == 0xffff) CHECK(size == 76); // Largest full-mask record, not out-of-layout.
    return b;
}

static void checkOwned(Bytes bytes, const BlockLayout &layout, const Records &records)
{
    auto s = session(layout);
    auto info = envelope();
    auto result = ValidateCodecFrame(bytes.data(), bytes.size(), s, info);
    CHECK(result && result->payloadSize() == bytes.size());
    CHECK(result->bytes().size() == bytes.size() + 16);
    CHECK(std::equal(bytes.begin(), bytes.end(), result->bytes().begin()));
    CHECK(std::all_of(result->bytes().begin() + bytes.size(), result->bytes().end(),
                      [](uint8_t b) { return b == 0; }));
    std::vector<uint32_t> expected(layout.block_32x32_to_8x8_mapping.size(), UINT32_MAX);
    uint32_t offset = 8; // Byte cursor includes initial sequence; table uses WORDS.
    for (const auto &r : records) {
        CHECK(expected[r.first] == UINT32_MAX);
        expected[r.first] = offset / 4; offset += uint32_t(r.second.size());
    }
    CHECK(offset == bytes.size() && result->wordOffsets() == expected);
    const Bytes original = bytes;
    std::fill(bytes.begin(), bytes.end(), 0); Bytes().swap(bytes);
    info = {}; s.width = 0;
    CHECK(std::equal(original.begin(), original.end(), result->bytes().begin()));
    CHECK(result->frameInfo().frameId == 0xabcdef12);
    CHECK(result->frameInfo().rtpTimestamp == 0x12345678);
    CHECK(result->frameInfo().firstReceiveUs == 91 && result->frameInfo().deadlineUs == 100091);
    CHECK(result->frameInfo().metadata.hasHostProcessingLatency);
    CHECK(result->frameInfo().metadata.hostProcessingLatency == 1234);
    CHECK(result->session().width == uint32_t(layout.width));
    CHECK(result->session().height == uint32_t(layout.height));
    CHECK(result->session().range == 0 && result->session().siting == 0);
}

static void testGoldensAndSplits()
{
    BlockLayout layout;
    CHECK(layout.init(128, 128, ChromaSubsampling::Chroma420));
    const Bytes zero = serialize(layout, {});
    CHECK(zero == Zero);
    checkOwned(zero, layout, {});
    const Records one = {{0, Coefficient}};
    const Bytes sparseGolden = serialize(layout, one);
    CHECK(sparseGolden == Sparse);
    checkOwned(sparseGolden, layout, one);
    // Separated slots including the last. Entire records fit boundary16/24.
    const Records sparse = {{0, Coefficient}, {30, record(30, 1)}, {50, record(50, 1)}};
    Bytes expected = {0x7f, 0xc0, 0x1f, 0xd0, 3, 0, 0, 0};
    for (const auto &r : sparse) expected.insert(expected.end(), r.second.begin(), r.second.end());
    for (size_t boundary : {16u, 24u}) {
        const Bytes bytes = serialize(layout, sparse, boundary, boundary == 16 ? 4 : 3);
        CHECK(bytes == expected); // Split/concatenate is one complete pure codec frame.
        checkOwned(bytes, layout, sparse);
    }
}

static void testLayoutCountsAndMasks()
{
    // Manual sums by level4..0: 12+9+9+9+12=51; 12+9+9+36+27=93;
    // 48+108+360+1215+1530=3261; 3072+9216+36864+147456+196608=393216.
    const uint32_t cases[][3] = {{2, 2, 51}, {128, 128, 51}, {130, 130, 93},
                                {1920, 1080, 3261}, {16384, 16384, 393216}};
    for (const uint32_t *c : cases) {
        BlockLayout layout;
        CHECK(layout.init(int(c[0]), int(c[1]), ChromaSubsampling::Chroma420));
        CHECK(layout.block_32x32_to_8x8_mapping.size() == c[2]);
        CHECK(uint32_t(layout.block_count_32x32) == c[2]);
        checkOwned(serialize(layout, {}), layout, {});
        if (c[0] == 128 || c[0] == 16384) continue;
        bool seen[5][5] = {};
        for (size_t i = 0; i < layout.block_32x32_to_8x8_mapping.size(); ++i) {
            const auto &m = layout.block_32x32_to_8x8_mapping[i];
            const int w = m.block_width_8x8, h = m.block_height_8x8;
            CHECK(w >= 1 && w <= 4 && h >= 1 && h <= 4);
            if (seen[w][h]) continue;
            seen[w][h] = true;
            uint16_t mask = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) mask = uint16_t(mask | (1u << (4 * y + x)));
            const Records good = {{uint32_t(i), record(uint32_t(i), mask)}};
            checkOwned(serialize(layout, good), layout, good);
            if (mask == 0xffff) continue; // No outside bit exists for full4x4.
            unsigned outside = 0;
            while ((mask & (1u << outside)) != 0) ++outside;
            const Records bad = {{uint32_t(i), record(uint32_t(i), uint16_t(mask | (1u << outside)))}};
            const Bytes bytes = serialize(layout, bad); // Structurally complete payload.
            CHECK(!ValidateCodecFrame(bytes.data(), bytes.size(), session(layout), envelope()));
        }
        CHECK(seen[4][4]); // Maximum ballot was exercised at a legitimate mapping.
    }
}

static void testLiteralMutations()
{
    BlockLayout layout;
    CHECK(layout.init(128, 128, ChromaSubsampling::Chroma420));
    const auto s = session(layout);
    for (const Bytes &literal : {Zero, Sparse}) {
        for (size_t length = 0; length < literal.size(); ++length)
            CHECK(!ValidateCodecFrame(literal.data(), length, s, envelope()));
        for (size_t byte : {0u, 2u, 4u, 7u}) { // Width, height, count, extended code.
            Bytes bad = literal; bad[byte] ^= 1;
            CHECK(!ValidateCodecFrame(bad.data(), bad.size(), s, envelope()));
        }
    }
    for (size_t byte : {3u, 11u}) { // Sequence mismatch on either side.
        Bytes bad = Sparse; bad[byte] ^= 0x10;
        CHECK(!ValidateCodecFrame(bad.data(), bad.size(), s, envelope()));
    }
    Bytes duplicate = Sparse;
    duplicate.insert(duplicate.end(), Coefficient.begin(), Coefficient.end());
    CHECK(ValidateCodecFrame(duplicate.data(), duplicate.size(), s, envelope()));
    duplicate.back() ^= 1; // Conflicting duplicate even in unconstrained padding.
    CHECK(!ValidateCodecFrame(duplicate.data(), duplicate.size(), s, envelope()));
}

int main()
{
    testGoldensAndSplits();
    testLayoutCountsAndMasks();
    testLiteralMutations();
    std::puts("pinned CPU packetizer oracle checks passed (synthetic coefficients)");
    return 0;
}
