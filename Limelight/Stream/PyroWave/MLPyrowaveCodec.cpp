// Copyright (c) 2025 Hans-Kristian Arntzen
// Copyright (c) 2026 AlkaidLab contributors
// SPDX-License-Identifier: MIT AND GPL-3.0-only
// Layout/payload algorithm derived from Foundation third-party/pyrowave at
// cc91cbf6363721d19d135d05ca8e3d8665ea6afd: metal/pyrowave_bitstream.{hpp,cpp},
// bitstream/bitstream.md, pyrowave_encoder.cpp::validate_bitstream and
// shaders/{block_packing,wavelet_dequant}.comp. Independent safe CPU parser;
// no upstream parser, bitfield structs, quant shifts or graphics ABI are used.
/* Original algorithm's MIT notice:
 * Copyright (c) 2025 Hans-Kristian Arntzen
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * AlkaidLab fork changes are GPL-3.0-only; MIT AND GPL-3.0-only denotes
 * combined coverage, not relicensing of unchanged MIT portions.
 */
#include "MLPyrowaveCodec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace MLPyrowave {
namespace {
constexpr uint64_t InputLimit = 16 * 1024 * 1024;
constexpr uint64_t StorageLimit = 32 * 1024 * 1024;

uint32_t read32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t read16(const uint8_t *p)
{
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

unsigned popcount(uint32_t bits)
{
#if defined(__clang__) || defined(__GNUC__)
    return unsigned(__builtin_popcountll(bits));
#else
    unsigned count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
#endif
}

bool add(uint64_t a, uint64_t b, uint64_t &out)
{
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}

bool multiply(uint64_t a, uint64_t b, uint64_t &out)
{
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}

} // namespace
bool IsSupportedCodecSession(const CodecSession &s) noexcept
{
    return s.width >= 2 && s.width <= 16384 && (s.width & 1) == 0 &&
        s.height >= 2 && s.height <= 16384 && (s.height & 1) == 0 &&
        s.bitDepth == 8 && s.chroma == 0 && s.primaries == 0 &&
        s.transfer == 0 && s.transform == 0 && s.range <= 1 && s.siting <= 1;
}

namespace {
uint64_t aligned(uint32_t dimension)
{
    // IsSupportedCodecSession precedes this: dimension+31 cannot overflow uint64_t.
    return std::max<uint64_t>(128, (uint64_t(dimension) + 31) & ~uint64_t(31));
}

bool storageBudget(uint64_t maskCapacity, uint64_t offsetCapacity, size_t bytes)
{
    uint64_t masks, offsets, total;
    // Include cached masks, frame/validator objects and allocation bookkeeping.
    const uint64_t overhead = sizeof(ValidatedCodecFrame) + sizeof(CodecValidator) + 1024;
    return multiply(maskCapacity, 2, masks) && multiply(offsetCapacity, 4, offsets) &&
        add(uint64_t(bytes), masks, total) && add(total, offsets, total) &&
        add(total, overhead, total) && total <= StorageLimit &&
        total <= std::numeric_limits<size_t>::max();
}

bool layoutBudget(const CodecSession &s, uint64_t &capacity)
{
    capacity = 0;
    for (int level = 4; level >= 0; --level) {
        const uint64_t w = aligned(s.width) >> (level + 1);
        const uint64_t h = aligned(s.height) >> (level + 1);
        const uint64_t bands = level == 4 ? 12 : (level == 0 ? 3 : 9);
        uint64_t blocks, increment;
        if (!multiply((w + 31) / 32, (h + 31) / 32, blocks) ||
            !multiply(blocks, bands, increment) || !add(capacity, increment, capacity))
            return false;
    }
    return capacity <= UINT32_MAX && storageBudget(capacity, capacity, 16);
}

void appendBandMasks(uint64_t w, uint64_t h, std::vector<uint16_t> &masks,
                     size_t &next)
{
    const uint64_t blocks8x = (w + 7) / 8, blocks8y = (h + 7) / 8;
    for (uint64_t y = 0; y < (h + 31) / 32; ++y) {
        for (uint64_t x = 0; x < (w + 31) / 32; ++x) {
            const unsigned cols = unsigned(std::min<uint64_t>(4, blocks8x - 4 * x));
            const unsigned rows = unsigned(std::min<uint64_t>(4, blocks8y - 4 * y));
            uint16_t mask = 0;
            for (unsigned row = 0; row < rows; ++row)
                mask = uint16_t(mask | (((1u << cols) - 1) << (4 * row)));
            masks[next++] = mask;
        }
    }
}

void fillMasks(const CodecSession &s, std::vector<uint16_t> &masks)
{
    size_t next = 0;
    for (int level = 4; level >= 0; --level) {
        for (unsigned component = 0; component < 3; ++component) {
            if (level == 0 && component != 0) continue;
            for (unsigned band = level == 4 ? 0 : 1; band < 4; ++band)
                appendBandMasks(aligned(s.width) >> (level + 1),
                                aligned(s.height) >> (level + 1), masks, next);
        }
    }
}

bool sequenceMatches(const uint8_t *input, const CodecSession &s)
{
    const uint32_t w0 = read32(input), w1 = read32(input + 4);
    // All code/color bits 24..29 must be zero for negotiated SDR 709/420.
    return (w0 >> 31) == 1 && (w0 & 0x3fff) + 1 == s.width &&
        ((w0 >> 14) & 0x3fff) + 1 == s.height &&
        (w1 & 0x3f000000) == 0 && (w1 >> 30) == (s.range | (uint32_t(s.siting) << 1));
}

bool validPayload(const uint8_t *record, size_t size, uint16_t ballot)
{
    const unsigned n = popcount(ballot);
    size_t cursor = 8 + 3 * n;
    if (cursor > size) return false;
    unsigned significant = 0;
    for (unsigned block = 0; block < n; ++block) {
        const uint16_t control = read16(record + 8 + 2 * block);
        const unsigned q = record[8 + 2 * n + block] & 15;
        // Dequant ignores zero control, but its offset scan counts q planes.
        if (control == 0 && q != 0) return false;
        for (unsigned j = 0; j < 8; ++j) {
            const unsigned planes = q + ((control >> (2 * j)) & 3);
            uint32_t combined = 0;
            for (unsigned plane = 0; plane < planes; ++plane) {
                if (cursor >= size) return false;
                combined |= record[cursor++];
            }
            significant += popcount(combined);
        }
    }
    const size_t signs = (significant + 7) / 8; // Pooled across the entire 32x32.
    if (signs > size - cursor) return false;
    // Unused sign bits and byte padding are deliberately unconstrained.
    return (cursor + signs + 3) / 4 == size / 4;
}

bool scanRecords(const uint8_t *input, size_t length,
                 const std::vector<uint16_t> &masks, std::vector<uint32_t> &wordOffsets)
{
    const uint32_t sequence = (read32(input) >> 28) & 7;
    const uint32_t total = read32(input + 4) & 0xffffff;
    uint32_t unique = 0;
    size_t cursor = 8;
    while (cursor < length) {
        const size_t remaining = length - cursor;
        if (remaining < 8) return false;
        const uint8_t *record = input + cursor;
        const uint32_t w0 = read32(record), w1 = read32(record + 4);
        if ((w0 >> 31) != 0) {
            if (std::memcmp(record, input, 8) != 0) return false;
            cursor += 8;
            continue;
        }
        const size_t size = ((w0 >> 16) & 0xfff) * 4;
        // Length/progress and payload checks MUST precede duplicate handling.
        if (size < 8 || size > remaining || ((w0 >> 28) & 7) != sequence)
            return false;
        const uint32_t index = w1 >> 8;
        const uint16_t ballot = uint16_t(w0);
        // Encoder encode_quant asserts exponent 0..20 => codes 0..167.
        if (index >= wordOffsets.size() || (w1 & 255) > 167 ||
            (ballot & ~masks[index]) != 0 || !validPayload(record, size, ballot))
            return false;
        const uint32_t previous = wordOffsets[index];
        if (previous != UINT32_MAX) {
            const size_t previousByteOffset = size_t(previous) * 4;
            const size_t previousSize = ((read32(input + previousByteOffset) >> 16) & 0xfff) * 4;
            if (previousSize != size || std::memcmp(input + previousByteOffset, record, size) != 0)
                return false;
        } else {
            wordOffsets[index] = uint32_t(cursor / 4); // Aligned progress; input cap bounds narrowing.
            ++unique;
        }
        cursor += size; // At least 8; bounded by remaining above.
    }
    return unique == total;
}
} // namespace

ValidatedCodecFrame::ValidatedCodecFrame(CodecBytes &&bytes,
    std::vector<uint32_t> &&wordOffsets, const CodecSession &session,
    const MLPyrowaveFrameInfo &info)
    : bytes_(std::move(bytes)), wordOffsets_(std::move(wordOffsets)), session_(session), info_(info)
{
}

CodecValidator::CodecValidator(const CodecSession &session, std::vector<uint16_t> &&masks)
    : session_(session), masks_(std::move(masks))
{
}

std::unique_ptr<CodecValidator> CodecValidator::Create(const CodecSession &session) noexcept
{
    try {
        uint64_t capacity;
        if (!IsSupportedCodecSession(session) || !layoutBudget(session, capacity))
            return nullptr;
        std::vector<uint16_t> masks(static_cast<size_t>(capacity), 0);
        if (!storageBudget(masks.capacity(), capacity, 16)) return nullptr;
        fillMasks(session, masks);
        return std::unique_ptr<CodecValidator>(new CodecValidator(session, std::move(masks)));
    } catch (const std::bad_alloc &) {
        return nullptr;
    } catch (const std::length_error &) {
        return nullptr;
    }
}

bool CodecValidator::Validate(const uint8_t *input, size_t length, size_t storageBytes,
    const MLPyrowaveFrameInfo &info, std::vector<uint32_t> &offsets) const
{
    if (input == nullptr || length < 8 || length > InputLimit || info.frameId == 0 ||
        info.deadlineUs < info.firstReceiveUs ||
        !storageBudget(masks_.capacity(), masks_.size(), storageBytes) ||
        !sequenceMatches(input, session_) || (read32(input + 4) & 0xffffff) > masks_.size())
        return false;
    offsets.assign(masks_.size(), UINT32_MAX);
    return storageBudget(masks_.capacity(), offsets.capacity(), storageBytes) &&
        scanRecords(input, length, masks_, offsets);
}

std::unique_ptr<const ValidatedCodecFrame> CodecValidator::ValidateOwned(
    CodecBytes &&bytes, size_t payloadLength, const MLPyrowaveFrameInfo &info) const
{
    try {
        if (payloadLength < 8 || payloadLength > InputLimit ||
            bytes.size() != payloadLength + 16) return nullptr;
        std::vector<uint32_t> offsets;
        if (!Validate(bytes.data(), payloadLength, bytes.capacity(), info, offsets)) return nullptr;
        std::fill(bytes.begin() + payloadLength, bytes.end(), uint8_t(0));
        return std::unique_ptr<const ValidatedCodecFrame>(new ValidatedCodecFrame(
            std::move(bytes), std::move(offsets), session_, info));
    } catch (const std::bad_alloc &) {
        return nullptr;
    } catch (const std::length_error &) {
        return nullptr;
    }
}

std::unique_ptr<const ValidatedCodecFrame> ValidateCodecFrame(
    const uint8_t *input, size_t length, const CodecSession &session,
    const MLPyrowaveFrameInfo &info)
{
    try {
        if (input == nullptr || length < 8 || length > InputLimit) return nullptr;
        auto validator = CodecValidator::Create(session);
        std::vector<uint32_t> offsets;
        if (!validator || !validator->Validate(input, length, length + 16, info, offsets))
            return nullptr;
        // Only a completely validated transaction earns an owned input copy.
        CodecBytes bytes(length + 16);
        std::memcpy(bytes.data(), input, length);
        std::fill(bytes.begin() + length, bytes.end(), uint8_t(0));
        return std::unique_ptr<const ValidatedCodecFrame>(new ValidatedCodecFrame(
            std::move(bytes), std::move(offsets), session, info));
    } catch (const std::bad_alloc &) {
        return nullptr;
    } catch (const std::length_error &) {
        return nullptr;
    }
}
} // namespace MLPyrowave
