// SPDX-License-Identifier: GPL-3.0-only
#include "MLPyrowaveReceiver.hpp"
#include <new>
#include <stdexcept>
#include <utility>

namespace MLPyrowave {
Receiver::Receiver(const CodecSession &session, uint32_t maxPacketSize,
    uint64_t (*clock)(void *), void *clockContext)
    : session_(session), maxPacketSize_(maxPacketSize), clock_(clock),
      clockContext_(clockContext), reassembly_(MLPyrowaveReassemblyCreate())
{
}

std::unique_ptr<Receiver> Receiver::Create(const CodecSession &session,
    uint32_t maxPacketSize, uint64_t (*clock)(void *), void *clockContext) noexcept
{
    if (!IsSupportedCodecSession(session) || clock == nullptr ||
        maxPacketSize <= 128 || maxPacketSize > 65536) return nullptr;
    try {
        std::unique_ptr<Receiver> receiver(new Receiver(session, maxPacketSize, clock, clockContext));
        if (receiver->reassembly_ == nullptr) return nullptr;
        return receiver;
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

Receiver::~Receiver()
{
    Stop();
    MLPyrowaveReassemblyDestroy(reassembly_);
}

void Receiver::Discard() noexcept
{
    MLPyrowaveReassemblyDiscardFrame(reassembly_);
    activeId_ = 0;
    deadline_ = 0;
    outerTimestamp_ = 0;
    presentation_ = 0;
}

void Receiver::Stop() noexcept
{
    stopped_ = true;
    Discard();
}

bool Receiver::Sample(uint64_t &now) noexcept
{
    now = clock_(clockContext_);
    if (sampled_ && now < lastClock_) {
        Discard();
        return false;
    }
    sampled_ = true;
    lastClock_ = now;
    return true;
}

std::unique_ptr<const ReceivedFrame> Receiver::Receive(
    const LI_PYROWAVE_RECEIVED_PACKET &packet) noexcept
{
    if (stopped_) return nullptr;
    uint64_t now;
    if (!Sample(now)) return nullptr;
    if (now < packet.receiveTimeUs) { Discard(); return nullptr; }
    // Even an incomplete frame must retire on local-clock expiry, not just
    // when the next packet's earlier receiveTimeUs reaches the deadline.
    if (activeId_ != 0 && now >= deadline_) Discard();
    try {
        LI_PYROWAVE_PACKET_HEADER header;
        const uint8_t *payload;
        if (packet.data == nullptr || packet.length > maxPacketSize_ ||
            LiPyrowaveParsePacket(packet.data, packet.length, &header, &payload) !=
                LI_PYROWAVE_PACKET_OK || header.frameId != packet.frameId ||
            (header.frameId == activeId_ && packet.rtpTimestamp != outerTimestamp_)) {
            Discard();
            return nullptr;
        }
        const auto status = MLPyrowaveReassemblyPush(reassembly_, packet.data,
                                                    packet.length, packet.receiveTimeUs);
        if (status != LI_PYROWAVE_REASSEMBLY_ACCEPTED &&
            status != LI_PYROWAVE_REASSEMBLY_COMPLETE) {
            if (status < 0) Discard();
            return nullptr; // Duplicate/stale cannot replace the timing snapshot.
        }
        if (header.frameId != activeId_) {
            activeId_ = header.frameId;
            outerTimestamp_ = packet.rtpTimestamp;
            presentation_ = packet.presentationTimeUs;
            // Strict Push has already checked first-arrival addition overflow.
            deadline_ = packet.receiveTimeUs + UINT64_C(100000);
        }
        if (now >= deadline_) { Discard(); return nullptr; }
        if (status != LI_PYROWAVE_REASSEMBLY_COMPLETE) return nullptr;
        if (header.codecPayloadLength > 16u * 1024u * 1024u) {
            Discard(); return nullptr;
        }
        std::vector<uint8_t> scratch(header.codecPayloadLength);
        if (!Sample(now)) return nullptr;
        size_t length = 0;
        MLPyrowaveFrameInfo ownedInfo = {};
        if (MLPyrowaveReassemblyCopyFrame(reassembly_, now, scratch.data(), scratch.size(),
                &length, &ownedInfo) != LI_PYROWAVE_REASSEMBLY_COMPLETE) {
            Discard(); return nullptr;
        }
        // Copy retires the watermark before validation/allocation can fail.
        auto codec = ValidateCodecFrame(scratch.data(), length, session_, ownedInfo);
        if (!codec) { Discard(); return nullptr; }
        std::unique_ptr<const ReceivedFrame> result(new ReceivedFrame{
            std::move(codec), outerTimestamp_, presentation_});
        if (!Sample(now) || now >= ownedInfo.deadlineUs) {
            Discard(); return nullptr;
        }
        Discard();
        return result;
    } catch (const std::bad_alloc &) {
        Discard(); return nullptr;
    } catch (const std::length_error &) {
        Discard(); return nullptr;
    }
}
} // namespace MLPyrowave
