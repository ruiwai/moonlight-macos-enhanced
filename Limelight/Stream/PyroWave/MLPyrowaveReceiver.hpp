// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_RECEIVER_HPP
#define ML_PYROWAVE_RECEIVER_HPP

#include "MLPyrowaveCodec.hpp"
#include "Limelight.h" // Common public API, not Limelight-internal.h.

namespace MLPyrowave {
struct ReceivedFrame final {
    std::unique_ptr<const ValidatedCodecFrame> codec;
    uint32_t outerRtpTimestamp;
    uint64_t presentationTimeUs;
};

// Serialized CPU-only raw-descriptor transaction; no queue or retained packets.
// Clock/context are borrowed, must outlive Receiver, must not throw, and must
// share receiveTimeUs's monotonic microsecond epoch (app: LiGetMicroseconds).
// Stop permanently disables admission. Caller owns every returned frame;
// future coordinator must bound waiting ownership and GPU leases separately.
// Reassembly's budget already includes this receiver's one <=16 MiB scratch.
// This receiver does NOT certify an aggregate GPU/session allocation budget.
class Receiver final {
public:
    static std::unique_ptr<Receiver> Create(const CodecSession &session,
        uint32_t maxPacketSize, uint64_t (*clock)(void *), void *clockContext) noexcept;
    ~Receiver();
    std::unique_ptr<const ReceivedFrame> Receive(const LI_PYROWAVE_RECEIVED_PACKET &) noexcept;
    void Stop() noexcept;
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;

private:
    Receiver(const CodecSession &, uint32_t, uint64_t (*)(void *), void *);
    void Discard() noexcept;
    bool Sample(uint64_t &now) noexcept;
    const CodecSession session_;
    const uint32_t maxPacketSize_;
    uint64_t (*const clock_)(void *);
    void *const clockContext_;
    MLPyrowaveReassembly *const reassembly_;
    bool stopped_ = false, sampled_ = false;
    uint64_t lastClock_ = 0, deadline_ = 0;
    uint32_t activeId_ = 0, outerTimestamp_ = 0;
    uint64_t presentation_ = 0;
};
} // namespace MLPyrowave
#endif
