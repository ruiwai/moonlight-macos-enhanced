// Copyright (c) 2025 Hans-Kristian Arntzen
// Copyright (c) 2026 AlkaidLab contributors
// SPDX-License-Identifier: MIT AND GPL-3.0-only
#ifndef ML_PYROWAVE_CODEC_HPP
#define ML_PYROWAVE_CODEC_HPP

#include "MLPyrowaveReassembly.h"
#include "MLPyrowaveBytes.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace MLPyrowave {
struct CodecSession {
    uint32_t width = 0, height = 0;
    // Wire enums: chroma 0=420/1=444; primaries/transform 0=709/1=2020;
    // transfer 0=709 (legacy sRGB alias), 1=PQ, 2=HLG (code=1).
    // range 0=full/1=limited; siting 0=center/1=left. /v1 only accepts
    // depth 8, chroma/primaries/transfer/transform 0, range/siting 0 or 1.
    uint8_t bitDepth = 8, chroma = 0, primaries = 0, transfer = 0;
    uint8_t transform = 0, range = 0, siting = 0;
};

class ValidatedCodecFrame;
bool IsSupportedCodecSession(const CodecSession &s) noexcept;
std::unique_ptr<const ValidatedCodecFrame> ValidateCodecFrame(
    const uint8_t *input, size_t length, const CodecSession &session,
    const MLPyrowaveFrameInfo &info);

// Session-scoped immutable layout cache; no frame/input storage is retained.
class CodecValidator final {
public:
    static std::unique_ptr<CodecValidator> Create(const CodecSession &) noexcept;
    // Exact size payloadLength+16, payload 8 bytes..16 MiB. Default storage charges
    // capacity (not size); pooled storage charges a whole 16 MiB + 64 KiB slot.
    // This charge, cached masks and offsets must fit the 32 MiB storage budget.
    // Precondition: first payloadLength bytes initialized; guards may be uninitialized.
    // Full validation precedes transfer; success preserves data() and zeros guards.
    // Failure returns null without moving bytes or publishing any frame.
    std::unique_ptr<const ValidatedCodecFrame> ValidateOwned(
        CodecBytes &&bytes, size_t payloadLength,
        const MLPyrowaveFrameInfo &info) const;
    CodecValidator(const CodecValidator &) = delete;
    CodecValidator &operator=(const CodecValidator &) = delete;

private:
    friend std::unique_ptr<const ValidatedCodecFrame> ValidateCodecFrame(
        const uint8_t *, size_t, const CodecSession &, const MLPyrowaveFrameInfo &);
    CodecValidator(const CodecSession &, std::vector<uint16_t> &&);
    bool Validate(const uint8_t *, size_t, size_t, const MLPyrowaveFrameInfo &,
                  std::vector<uint32_t> &) const;
    const CodecSession session_;
    const std::vector<uint16_t> masks_;
};

// Immutable CPU transaction, not a Metal ABI or a guarantee of GPU validity.
class ValidatedCodecFrame final {
public:
    ~ValidatedCodecFrame() = default;
    const CodecBytes &bytes() const { return bytes_; }
    const std::vector<uint32_t> &wordOffsets() const { return wordOffsets_; }
    const CodecSession &session() const { return session_; }
    const MLPyrowaveFrameInfo &frameInfo() const { return info_; }
    size_t payloadSize() const { return bytes_.size() - 16; }

private:
    friend class CodecValidator;
    friend std::unique_ptr<const ValidatedCodecFrame> ValidateCodecFrame(
        const uint8_t *, size_t, const CodecSession &, const MLPyrowaveFrameInfo &);
    ValidatedCodecFrame(CodecBytes &&bytes, std::vector<uint32_t> &&wordOffsets,
                        const CodecSession &session, const MLPyrowaveFrameInfo &info);
    ValidatedCodecFrame(const ValidatedCodecFrame &) = delete;
    ValidatedCodecFrame &operator=(const ValidatedCodecFrame &) = delete;
    CodecBytes bytes_; // Full input, then 16 initialized zero guard bytes.
    std::vector<uint32_t> wordOffsets_; // 4-byte WORD offsets; UINT32_MAX means absent.
    CodecSession session_;
    MLPyrowaveFrameInfo info_; // Full envelope ID, RTP time, receive/deadline, metadata.
};

// Caller supplies complete pure codec bytes and already validated envelope/TLV
// scalars, stable for the call; no borrowed storage survives. Nonzero frameId and
// deadline >= firstReceive required; no clock comparisons here. Null on any error,
// including bad_alloc/length_error; nothing is published before full success.
// Limits: input 8 bytes..16 MiB; even dimensions 2..16384; owned+temporary
// storage <=32 MiB. Guard initialization does not certify shader read bounds.
} // namespace MLPyrowave
#endif
