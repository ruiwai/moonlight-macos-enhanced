// Copyright (c) 2025 Hans-Kristian Arntzen
// Copyright (c) 2026 AlkaidLab contributors
// SPDX-License-Identifier: MIT AND GPL-3.0-only
// Synthetic LE fixtures only; pinned CPU packetizer oracle is in test_packetizer.cpp.
#include "MLPyrowaveCodec.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); } } while (0)

using namespace MLPyrowave;
using Bytes = CodecBytes;

static void put32(Bytes &b, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b[offset + i] = uint8_t(value >> (8 * i));
}

static void append(Bytes &to, const Bytes &from)
{
    to.insert(to.end(), from.begin(), from.end());
}

static CodecSession session(uint32_t dimension = 128)
{
    CodecSession s;
    s.width = s.height = dimension;
    return s;
}

static MLPyrowaveFrameInfo envelope()
{
    MLPyrowaveFrameInfo info = {};
    info.frameId = 0xabcde003; // Not the codec's short sequence 3.
    info.rtpTimestamp = 0x12345678;
    info.firstReceiveUs = 91;
    info.deadlineUs = 100091;
    info.metadata.hasHostProcessingLatency = true;
    info.metadata.hostProcessingLatency = 65535;
    return info;
}

static Bytes sequence(const CodecSession &s, uint32_t count = 1)
{
    Bytes b(8, 0);
    put32(b, 0, (s.width - 1) | ((s.height - 1) << 14) | (3u << 28) | (1u << 31));
    put32(b, 4, count | (uint32_t(s.range) << 30) | (uint32_t(s.siting) << 31));
    return b;
}

static Bytes record(uint32_t index = 0, uint8_t quant = 0, uint16_t ballot = 1,
                    unsigned words = 4)
{
    Bytes b(std::max<size_t>(8, words * 4), 0);
    put32(b, 0, ballot | (uint32_t(words) << 16) | (3u << 28));
    put32(b, 4, quant | (index << 8));
    if (words >= 4 && ballot == 1) {
        b[8] = 1; // control=1, q=0: one plane in subblock 0.
        b[11] = 1; // One significant coefficient, hence one pooled sign byte.
        b[12] = 0xff; // Unused sign bits and padding are ignored.
        b[13] = b[14] = b[15] = 0xa5;
    }
    return b;
}

static Bytes frame(const CodecSession &s, const Bytes &r = record())
{
    Bytes b = sequence(s);
    append(b, r);
    return b;
}

static std::unique_ptr<const ValidatedCodecFrame> validate(const Bytes &b,
    const CodecSession &s = session(), const MLPyrowaveFrameInfo &info = envelope())
{
    auto borrowed = ValidateCodecFrame(b.data(), b.size(), s, info);
    auto validator = CodecValidator::Create(s);
    if (!validator) { CHECK(!borrowed); return borrowed; }
    Bytes bytes = b;
    bytes.resize(b.size() + 16, 0xa5);
    auto owned = validator->ValidateOwned(std::move(bytes), b.size(), info);
    CHECK(bool(owned) == bool(borrowed)); // All existing malformed/duplicate fixtures too.
    if (owned) {
        CHECK(owned->bytes() == borrowed->bytes());
        CHECK(owned->wordOffsets() == borrowed->wordOffsets());
    }
    return borrowed;
}

static void testOverwriteBytes()
{
    Bytes filled(8, 0xa5);
    CHECK(std::all_of(filled.begin(), filled.end(), [](uint8_t b) { return b == 0xa5; }));
    filled.resize(16); // Initialize newly added bytes before any read/copy.
    std::fill(filled.begin() + 8, filled.end(), uint8_t(0xa5));
    CHECK(std::all_of(filled.begin(), filled.end(), [](uint8_t b) { return b == 0xa5; }));
    const Bytes good = frame(session());
    Bytes scratch(good.size() + 16);
    std::copy(good.begin(), good.end(), scratch.begin());
    std::fill(scratch.begin() + good.size(), scratch.end(), uint8_t(0xa5));
    const auto *allocation = scratch.data();
    Bytes moved = std::move(scratch);
    CHECK(moved.data() == allocation);
    auto validator = CodecValidator::Create(session()); CHECK(validator);
    auto result = validator->ValidateOwned(std::move(moved), good.size(), envelope());
    CHECK(result && result->bytes().data() == allocation);
    CHECK(result->payloadSize() == good.size());
    CHECK(std::equal(good.begin(), good.end(), result->bytes().begin()));
    CHECK(std::all_of(result->bytes().begin() + good.size(), result->bytes().end(),
        [](uint8_t b) { return b == 0; }));
}

static void testOwnedTransferAndCache()
{
    auto s = session();
    auto validator = CodecValidator::Create(s); CHECK(validator);
    const Bytes good = frame(s);
    Bytes bytes = good;
    bytes.resize(good.size() + 16, 0xa5);
    const auto *allocation = bytes.data();
    auto info = envelope();
    auto first = validator->ValidateOwned(std::move(bytes), good.size(), info);
    CHECK(first && first->bytes().data() == allocation);
    CHECK(first->payloadSize() == good.size() && first->bytes().size() == good.size() + 16);
    CHECK(std::equal(good.begin(), good.end(), first->bytes().begin()));
    CHECK(std::all_of(first->bytes().begin() + good.size(), first->bytes().end(),
        [](uint8_t byte) { return byte == 0; }));
    s.width = 0; // Cache owns its session snapshot, not the caller's storage.
    const Bytes zero = sequence(session(), 0);
    bytes = zero; bytes.resize(zero.size() + 16, 0xa5);
    info.frameId += 8; info.metadata = {};
    auto second = validator->ValidateOwned(std::move(bytes), zero.size(), info);
    CHECK(second && second->frameInfo().frameId == info.frameId);
    CHECK(!second->frameInfo().metadata.hasHostProcessingLatency);
    CHECK(std::all_of(second->wordOffsets().begin(), second->wordOffsets().end(),
        [](uint32_t offset) { return offset == UINT32_MAX; }));
    bytes = good; bytes.push_back(0); // Reject after a valid record was scanned.
    const size_t badLength = bytes.size(); bytes.resize(badLength + 16, 0xa5);
    const Bytes unchanged = bytes;
    CHECK(!validator->ValidateOwned(std::move(bytes), badLength, envelope()));
    CHECK(bytes == unchanged);
    validator.reset();
    CHECK(first->bytes().data() == allocation && first->wordOffsets()[0] == 2);
    CHECK(first->session().width == 128 && first->frameInfo().frameId == envelope().frameId);
    CHECK(first->frameInfo().metadata.hostProcessingLatency == 65535);
    CHECK(std::equal(good.begin(), good.end(), first->bytes().begin()));
    CHECK(second->payloadSize() == zero.size());
}

static void testOwnedBounds()
{
    CHECK(!CodecValidator::Create(CodecSession{}));
    auto validator = CodecValidator::Create(session()); CHECK(validator);
    const Bytes good = frame(session());
    for (size_t length : {size_t(0), size_t(7), size_t(16u * 1024 * 1024 + 1),
                          std::numeric_limits<size_t>::max()}) {
        Bytes bytes = good; bytes.resize(good.size() + 16, 0xa5);
        const Bytes unchanged = bytes;
        CHECK(!validator->ValidateOwned(std::move(bytes), length, envelope()));
        CHECK(bytes == unchanged);
    }
    for (size_t padding : {size_t(0), size_t(15), size_t(17)}) {
        Bytes bytes = good; bytes.resize(good.size() + padding, 0xa5);
        CHECK(!validator->ValidateOwned(std::move(bytes), good.size(), envelope()));
    }
    Bytes oversized = good; oversized.resize(good.size() + 16, 0xa5);
    oversized.reserve(32u * 1024 * 1024); // Size fits; capacity plus tables does not.
    const auto *allocation = oversized.data();
    const Bytes unchanged = oversized;
    CHECK(!validator->ValidateOwned(std::move(oversized), good.size(), envelope()));
    CHECK(oversized.data() == allocation && oversized == unchanged);
    Bytes bytes = good; bytes.resize(good.size() + 16, 0xa5);
    CHECK(validator->ValidateOwned(std::move(bytes), good.size(), envelope()));
}

static void testValidAndOwnership()
{
    for (uint32_t dimension : {2u, 128u, 130u}) {
        for (uint8_t range : {0, 1}) {
            for (uint8_t siting : {0, 1}) {
                CodecSession s = session(dimension);
                s.range = range; s.siting = siting;
                auto zero = validate(sequence(s, 0), s);
                CHECK(zero && zero->payloadSize() == 8);
                CHECK(std::all_of(zero->wordOffsets().begin(), zero->wordOffsets().end(),
                    [](uint32_t offset) { return offset == UINT32_MAX; }));
                Bytes b = frame(s);
                auto info = envelope();
                auto result = validate(b, s, info);
                CHECK(result && result->payloadSize() == b.size());
                CHECK(result->wordOffsets().size() == (dimension == 130 ? 93 : 51));
                CHECK(result->wordOffsets()[0] == 2 && result->wordOffsets()[1] == UINT32_MAX);
                CHECK(result->session().range == range && result->session().siting == siting);
                CHECK(result->session().width == dimension && result->session().height == dimension);
                CHECK(std::equal(b.begin(), b.end(), result->bytes().begin()));
                CHECK(std::all_of(result->bytes().begin() + b.size(), result->bytes().end(),
                    [](uint8_t byte) { return byte == 0; }));
                const auto old = info;
                std::fill(b.begin(), b.end(), 0); Bytes().swap(b);
                info = {}; s.width = 0;
                CHECK(result->frameInfo().frameId == old.frameId);
                CHECK(result->frameInfo().rtpTimestamp == old.rtpTimestamp);
                CHECK(result->frameInfo().firstReceiveUs == old.firstReceiveUs);
                CHECK(result->frameInfo().deadlineUs == old.deadlineUs);
                CHECK(result->frameInfo().metadata.hasHostProcessingLatency);
                CHECK(result->frameInfo().metadata.hostProcessingLatency == 65535);
                CHECK(result->bytes()[8] == 1 && result->session().width == dimension);
            }
        }
    }
}

static void testSequenceAndIdentity()
{
    const auto s = session();
    const Bytes good = frame(s);
    CHECK(!validate(record())); // Missing initial extended sequence.
    for (unsigned bit : {0u, 14u, 28u, 32u, 56u, 57u, 58u, 59u, 60u, 61u, 62u, 63u}) {
        Bytes bad = good;
        bad[bit / 8] ^= uint8_t(1u << (bit % 8));
        // A changed initial short sequence must conflict with the coefficient.
        CHECK(!validate(bad));
        Bytes repeat = good;
        append(repeat, Bytes(bad.begin(), bad.begin() + 8));
        CHECK(!validate(repeat));
    }
    Bytes repeat = good; append(repeat, sequence(s)); CHECK(validate(repeat));
    CHECK(!validate(sequence(s, 52))); // Capacity=51, before layout allocation.
    CHECK(!validate(sequence(s, 1))); // Missing declared record.
    Bytes count = good; put32(count, 4, 0); CHECK(!validate(count));
    count = good; put32(count, 4, 2); CHECK(!validate(count));
    auto info = envelope(); info.frameId = 0; CHECK(!validate(good, s, info));
    info = envelope(); info.deadlineUs = info.firstReceiveUs - 1;
    CHECK(!validate(good, s, info));
    info = envelope(); info.firstReceiveUs = info.deadlineUs = UINT64_MAX;
    CHECK(validate(good, s, info)); // No clock/overflow arithmetic.
    info.firstReceiveUs = info.deadlineUs = 0; info.frameId = 0x12340003;
    info.metadata = {};
    auto a = validate(good, s, info); CHECK(a);
    CHECK(!a->frameInfo().metadata.hasHostProcessingLatency);
    CHECK(a->frameInfo().metadata.hostProcessingLatency == 0);
    info.frameId += 8; auto b = validate(good, s, info); CHECK(b);
    CHECK(a->frameInfo().frameId != b->frameInfo().frameId);
}

static void testSessionAndBounds()
{
    const Bytes b = frame(session());
    for (uint32_t dimension : {0u, 1u, 3u, 129u, 16385u, UINT32_MAX}) {
        auto s = session(); s.width = dimension; CHECK(!validate(b, s));
        s = session(); s.height = dimension; CHECK(!validate(b, s));
    }
    for (unsigned field = 0; field < 7; ++field) {
        auto s = session();
        uint8_t *fields[] = {&s.bitDepth, &s.chroma, &s.primaries, &s.transfer,
                            &s.transform, &s.range, &s.siting};
        *fields[field] = field == 0 ? 10 : (field >= 5 ? 2 : 1);
        CHECK(!validate(b, s));
    }
    auto hlg = session(); hlg.transfer = 2; CHECK(!validate(b, hlg));
    auto maximum = session(16384);
    CHECK(validate(sequence(maximum, 0), maximum)); // CPU-only layout preflight.
    CHECK(!ValidateCodecFrame(nullptr, b.size(), session(), envelope()));
    const uint8_t sentinel = 0;
    CHECK(!ValidateCodecFrame(&sentinel, 16u * 1024 * 1024 + 1, session(), envelope()));
    CHECK(!ValidateCodecFrame(&sentinel, std::numeric_limits<size_t>::max(), session(), envelope()));
}

static void testDuplicatesAndRecordHeaders()
{
    const auto s = session();
    Bytes b = frame(s); append(b, record());
    auto same = validate(b); CHECK(same && same->wordOffsets()[0] == 2);
    for (unsigned words : {0u, 1u, 2u, 3u, 5u, 4095u}) {
        Bytes bad = frame(s); append(bad, record(0, 0, 1, words)); CHECK(!validate(bad));
    }
    for (size_t offset : {size_t(4), size_t(8), size_t(10), size_t(11), size_t(12), size_t(15)}) {
        Bytes r = record(); r[offset] ^= 1;
        Bytes bad = frame(s); append(bad, r); CHECK(!validate(bad));
    }
    CHECK(validate(frame(s, record(0, 167))));
    for (uint8_t q : {168, 200, 255}) CHECK(!validate(frame(s, record(0, q))));
    CHECK(!validate(frame(s, record(51))));
    CHECK(!validate(frame(s, record(0xffffff))));
    Bytes mismatch = record(); mismatch[3] ^= 0x10;
    CHECK(!validate(frame(s, mismatch)));
    CHECK(validate(frame(s, record(0, 0, 0, 2)))); // Ballot0 counts once.
    CHECK(!validate(frame(s, record(0, 0, 0, 3))));
    // Header-only record at last layout slot proves sparse index semantics.
    auto last = validate(frame(s, record(50, 0, 0, 2)));
    CHECK(last && last->wordOffsets()[50] == 2 && last->wordOffsets()[0] == UINT32_MAX);
}

static void testWordOffsetsAndNonFirstDuplicates()
{
    Bytes middle = record(3);
    middle[8] = middle[10] = 0; // Zero control/q0: 12-byte record, including padding.
    middle.resize(12); put32(middle, 0, 1 | (3u << 16) | (3u << 28));
    Bytes b = sequence(session(), 3);
    append(b, record(7)); append(b, middle); append(b, record(50, 0, 0, 2));
    std::vector<uint32_t> expected(51, UINT32_MAX);
    expected[7] = 2; expected[3] = 6; expected[50] = 9; // Byte starts 8/24/36.
    auto result = validate(b);
    CHECK(result && result->wordOffsets() == expected && result->payloadSize() == 44);
    append(b, middle); // Identical non-first record: preserve its original offset.
    result = validate(b);
    CHECK(result && result->wordOffsets() == expected && result->payloadSize() == 56);
    for (size_t byte : {size_t(4), size_t(11)}) {
        Bytes conflict = middle; conflict[byte] ^= 1; // Quant or ignored padding.
        Bytes bad = b; append(bad, conflict); CHECK(!validate(bad));
    }
    append(b, record(3)); CHECK(!validate(b)); // Same index, different valid length.
}

static void testPayloadAndGeometry()
{
    const auto s = session();
    const Bytes good = frame(s);
    for (size_t length = 0; length < good.size(); ++length)
        CHECK(!ValidateCodecFrame(good.data(), length, s, envelope()));
    const Bytes zero = sequence(s, 0);
    for (size_t length = 0; length < zero.size(); ++length)
        CHECK(!ValidateCodecFrame(zero.data(), length, s, envelope()));
    Bytes trailing = good; trailing.push_back(0); CHECK(!validate(trailing));
    CHECK(!validate(frame(s, record(0, 0, 2)))); // LL4 is only one 8x8.
    CHECK(!validate(frame(s, record(0, 0, 0xffff, 2))));
    CHECK(!validate(frame(s, record(0, 0, 1, 2)))); // Missing control/qscale.
    Bytes plane = record(); plane[8] = 0xff; plane[9] = 0xff;
    CHECK(!validate(frame(s, plane))); // More planes than remaining bytes.
    Bytes signs = record(); signs[8] = 0x55; signs[9] = 0x55;
    signs.resize(20, 0xff); put32(signs, 0, 1 | (5u << 16) | (3u << 28));
    std::fill(signs.begin() + 11, signs.begin() + 19, 0xff);
    CHECK(!validate(frame(s, signs))); // 8 plane bytes, 64 signs: needs 28 bytes.
    Bytes excess = record(); excess.resize(20, 0); put32(excess, 0, 1 | (5u << 16) | (3u << 28));
    CHECK(!validate(frame(s, excess)));
    Bytes ambiguous = record(); ambiguous[8] = 0; ambiguous[10] = 1;
    CHECK(!validate(frame(s, ambiguous)));
    Bytes empty = record(); empty[8] = empty[10] = 0;
    empty.resize(12); put32(empty, 0, 1 | (3u << 16) | (3u << 28));
    CHECK(validate(frame(s, empty))); // Zero control with q0 is unambiguous.
    Bytes huge = record(0, 0, 1, 4095); CHECK(!validate(frame(s, huge)));
    // 130 aligns to160; level0 is80x80: last32 block has2x2 legal8 blocks.
    auto edgeSession = session(130);
    CHECK(validate(frame(edgeSession, record(74, 0, 0x33, 5)), edgeSession));
    CHECK(!validate(frame(edgeSession, record(74, 0, 0x40, 3)), edgeSession));
    auto held = validate(good); CHECK(held);
    CHECK(!validate(trailing)); CHECK(held->payloadSize() == good.size());
    Bytes unaligned(1, 0); append(unaligned, good);
    CHECK(ValidateCodecFrame(unaligned.data() + 1, good.size(), s, envelope()));
}

static void testPooledSignsAndPlanes()
{
    // Five active 8x8 blocks: metadata23 +planes5 +pooledSigns1 =29 =>8 words.
    // Independently rounding signs per8 block would incorrectly need9 words.
    Bytes pooled = record(30, 0, 0x1f, 8); // First level1 band: full 4x4 ballot.
    for (unsigned i = 0; i < 5; ++i) {
        pooled[8 + 2 * i] = 1;
        pooled[23 + i] = 1;
    }
    CHECK(validate(frame(session(), pooled)));
    Bytes two = sequence(session(), 2); append(two, record()); append(two, pooled);
    auto result = validate(two);
    CHECK(result && result->wordOffsets()[0] == 2 && result->wordOffsets()[30] == 6);
    // OR plane bytes, don't count their bits separately: 3 identical planes per
    // subblock give8 significant coefficients total, not24 (would need10 words).
    Bytes planes = record(0, 0, 1, 6);
    planes[8] = planes[9] = 0xff;
    planes.resize(36, 1); // 8+3meta+24planes+1sign=36 bytes,9 words.
    put32(planes, 0, 1 | (9u << 16) | (3u << 28));
    std::fill(planes.begin() + 11, planes.begin() + 35, 1);
    CHECK(validate(frame(session(), planes)));
    planes[10] = 0xf0; // High qscale nibble doesn't change plane counts.
    CHECK(validate(frame(session(), planes)));
    Bytes qPlanes = record(0, 0, 1, 6);
    qPlanes[10] = 1; // control1 +q1 gives2 planes, then7 subblocks of1 plane.
    std::fill(qPlanes.begin() + 11, qPlanes.begin() + 20, 1);
    CHECK(validate(frame(session(), qPlanes))); // 11meta+9planes+1sign =>6 words.
}

int main()
{
    testOverwriteBytes();
    testOwnedTransferAndCache();
    testOwnedBounds();
    testValidAndOwnership();
    testSequenceAndIdentity();
    testSessionAndBounds();
    testDuplicatesAndRecordHeaders();
    testWordOffsetsAndNonFirstDuplicates();
    testPayloadAndGeometry();
    testPooledSignsAndPlanes();
    std::puts("codec synthetic checks passed");
    return 0;
}
