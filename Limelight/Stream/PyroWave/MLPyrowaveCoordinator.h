// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_COORDINATOR_H
#define ML_PYROWAVE_COORDINATOR_H

#import <Foundation/Foundation.h>
#include "Limelight.h" // Public Common v2 descriptor only.

#ifdef __cplusplus
namespace MLPyrowave { struct ReceivedFrame; }
#endif

NS_ASSUME_NONNULL_BEGIN
typedef struct {
    uint32_t width, height, maxPacketSize;
    uint8_t range, siting;
    uint64_t generation;
} MLPyrowaveConfiguration;
typedef uint64_t (*MLPyrowaveClock)(void * _Nullable context);

// Immutable CPU owner; taking a frame retains it independently of stop/dealloc.
// Consumers must reject stale generations against their current session.
@interface MLPyrowaveFrame : NSObject
@property(nonatomic, readonly) uint64_t generation;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
#ifdef __cplusplus
// Borrowed pointer: valid only while a strong reference to this wrapper lives.
- (const MLPyrowave::ReceivedFrame *)validatedFrame;
#endif
@end

// CPU-only handoff, not GPU validity or aggregate session/GPU budget proof.
// Exactly one queued owner; callers must separately bound all taken owners.
// Serialized synchronous submit/stop, nonblocking tryTake; no packet queue.
// Clock/context are borrowed, nonthrowing, must outlive this object and share
// receiveTimeUs's monotonic microsecond epoch. Regression permanently stops it.
// Generation must be nonzero; already-taken owners are NOT revoked by stop.
@interface MLPyrowaveCoordinator : NSObject
- (nullable instancetype)initWithConfiguration:(MLPyrowaveConfiguration)configuration
                                         clock:(nullable MLPyrowaveClock)clock
                                       context:(nullable void *)context;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
// Borrows descriptor/bytes synchronously; acknowledges only installed owned,
// complete, validated frames (Common v2 IGNORED or VALIDATED_FRAME, not GPU).
- (int)submitPacket:(nullable const LI_PYROWAVE_RECEIVED_PACKET *)packet;
- (nullable MLPyrowaveFrame *)tryTakeFrame;
// Idempotent permanent local stop; no synchronous Common stop or GPU waits.
- (void)stop;
@end
NS_ASSUME_NONNULL_END
#endif
