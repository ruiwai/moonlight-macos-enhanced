// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#import "MLPyrowaveCoordinator.h"
#include "Limelight.h"

NS_ASSUME_NONNULL_BEGIN
// CPU app-boundary shape only, NOT GPU availability or session-budget proof.
FOUNDATION_EXPORT BOOL MLPyrowaveValidatePresentationConfiguration(
    MLPyrowaveConfiguration c, int frameRate);
// Explicit preference index 3 only; caller supplies actual preflight result.
// Mask policy is not GPU availability/correctness evidence.
FOUNDATION_EXPORT int MLPyrowaveOfferedFormats(int preference, BOOL preflightReady, int legacyFormats);

@protocol MLPyrowavePacketSink <NSObject>
// Synchronous borrow only; return the Common CPU validation result unchanged.
- (int)submitPyrowavePacket:(const LI_PYROWAVE_RECEIVED_PACKET *)packet;
@end

// Nonretaining context: caller MUST retain sink through Common cleanup/join.
// No TLS lookup, packet ownership transfer, async work or Common stop here.
// Nil sink returns an all-zero (disabled) descriptor.
FOUNDATION_EXPORT LI_PYROWAVE_PACKET_CALLBACKS MLPyrowaveCallbacksForSink(
    id<MLPyrowavePacketSink> _Nullable sink);
NS_ASSUME_NONNULL_END
