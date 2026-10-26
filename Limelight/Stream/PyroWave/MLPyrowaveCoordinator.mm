// SPDX-License-Identifier: GPL-3.0-only
#import "MLPyrowaveCoordinator.h"
#include "MLPyrowaveReceiver.hpp"
#include <os/lock.h>
#include <utility>

namespace {
struct ClockState {
    MLPyrowaveClock clock = nullptr;
    void *context = nullptr;
    uint64_t last = 0;
    bool sampled = false, regressed = false;
};
uint64_t Sample(void *context) noexcept
{
    auto &state = *static_cast<ClockState *>(context);
    const uint64_t now = state.clock(state.context);
    if (state.sampled && now < state.last) state.regressed = true;
    state.last = now;
    state.sampled = true;
    return now;
}
class ScopedLock final {
public:
    explicit ScopedLock(os_unfair_lock *lock, bool acquire = true) : lock_(lock)
    { if (acquire) os_unfair_lock_lock(lock_); }
    ~ScopedLock() { os_unfair_lock_unlock(lock_); }
    ScopedLock(const ScopedLock &) = delete;
    ScopedLock &operator=(const ScopedLock &) = delete;
private:
    os_unfair_lock *lock_;
};
} // namespace

@interface MLPyrowaveFrame () {
    std::unique_ptr<const MLPyrowave::ReceivedFrame> _frame;
}
- (instancetype)initWithFrame:(std::unique_ptr<const MLPyrowave::ReceivedFrame>)frame
                   generation:(uint64_t)generation;
@end

@implementation MLPyrowaveFrame
- (instancetype)initWithFrame:(std::unique_ptr<const MLPyrowave::ReceivedFrame>)frame
                   generation:(uint64_t)generation
{
    self = [super init];
    if (self) {
        _frame = std::move(frame);
        _generation = generation;
    }
    return self;
}
- (const MLPyrowave::ReceivedFrame *)validatedFrame { return _frame.get(); }
@end

@interface MLPyrowaveCoordinator () {
    os_unfair_lock _lock;
    ClockState _clock;
    std::unique_ptr<MLPyrowave::Receiver> _receiver;
    MLPyrowaveFrame *_pending;
    uint64_t _generation;
    bool _stopped;
}
- (void)stopLocked;
@end

@implementation MLPyrowaveCoordinator
- (instancetype)initWithConfiguration:(MLPyrowaveConfiguration)configuration
                                 clock:(MLPyrowaveClock)clock context:(void *)context
{
    self = [super init];
    if (!self || !clock || configuration.generation == 0) return nil;
    _lock = OS_UNFAIR_LOCK_INIT;
    _clock.clock = clock;
    _clock.context = context;
    MLPyrowave::CodecSession session;
    session.width = configuration.width;
    session.height = configuration.height;
    session.range = configuration.range;
    session.siting = configuration.siting;
    _receiver = MLPyrowave::Receiver::Create(session, configuration.maxPacketSize,
                                            Sample, &_clock);
    if (!_receiver) return nil;
    _generation = configuration.generation;
    return self;
}
- (void)stopLocked
{
    _stopped = true;
    _receiver->Stop();
    _pending = nil;
}
- (int)submitPacket:(const LI_PYROWAVE_RECEIVED_PACKET *)packet
{
    if (!packet) return LI_PYROWAVE_PACKET_IGNORED;
    ScopedLock lock(&_lock);
    if (_stopped) return LI_PYROWAVE_PACKET_IGNORED;
    auto frame = _receiver->Receive(*packet);
    if (_clock.regressed) {
        [self stopLocked];
        return LI_PYROWAVE_PACKET_IGNORED;
    }
    if (!frame) return LI_PYROWAVE_PACKET_IGNORED;
    MLPyrowaveFrame *wrapper = [[MLPyrowaveFrame alloc] initWithFrame:std::move(frame)
                                                        generation:_generation];
    const uint64_t now = Sample(&_clock); // Includes time spent allocating wrapper.
    if (_clock.regressed) {
        [self stopLocked];
        return LI_PYROWAVE_PACKET_IGNORED;
    }
    if (!wrapper) return LI_PYROWAVE_PACKET_IGNORED;
    const auto &info = [wrapper validatedFrame]->codec->frameInfo();
    if (now < info.firstReceiveUs || now >= info.deadlineUs)
        return LI_PYROWAVE_PACKET_IGNORED;
    _pending = wrapper; // Latest valid owner wins; invalid/incomplete preserves it.
    return LI_PYROWAVE_PACKET_VALIDATED_FRAME;
}
- (MLPyrowaveFrame *)tryTakeFrame
{
    if (!os_unfair_lock_trylock(&_lock)) return nil;
    ScopedLock lock(&_lock, false);
    if (_stopped) return nil;
    const uint64_t now = Sample(&_clock);
    if (_clock.regressed) { [self stopLocked]; return nil; }
    MLPyrowaveFrame *frame = _pending;
    _pending = nil;
    if (!frame) return nil;
    const auto &info = [frame validatedFrame]->codec->frameInfo();
    if (now < info.firstReceiveUs || now >= info.deadlineUs) return nil;
    return frame;
}
- (void)stop
{
    ScopedLock lock(&_lock);
    [self stopLocked];
}
@end
