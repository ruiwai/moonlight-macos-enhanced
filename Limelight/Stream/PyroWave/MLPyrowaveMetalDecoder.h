// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_METAL_DECODER_H
#define ML_PYROWAVE_METAL_DECODER_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "MLPyrowaveMetalKernels.h"
#import "MLPyrowaveCoordinator.h"

NS_ASSUME_NONNULL_BEGIN
typedef NS_ENUM(NSInteger, MLPyrowaveMetalSubmitResult) {
    MLPyrowaveMetalSubmitSubmitted,
    MLPyrowaveMetalSubmitBusy,
    MLPyrowaveMetalSubmitDropped,
    MLPyrowaveMetalSubmitStopped,
    MLPyrowaveMetalSubmitFailed,
};
// Synchronous, nonthrowing borrow ONLY. Encode all final readers here, on this
// command buffer; may presentDrawable, MUST NOT commit/enqueue/retain the buffer
// or planes, use them elsewhere, or reenter decoder control methods. End encoders
// before returning. NO discards the uncommitted buffer, including presentation.
typedef BOOL (^MLPyrowaveMetalPresentation)(id<MTLCommandBuffer> commandBuffer,
    id<MTLTexture> y, id<MTLTexture> cb, id<MTLTexture> cr);
// Only invoked for Submitted; possibly background, NOT display acknowledgement.
// Must be lifetime-safe, nonthrowing and avoid retaining codec/decoder/UI owners.
typedef void (^MLPyrowaveMetalCompletion)(BOOL success);

// Fixed generation/session SDR 709 8-bit 420. One GPU slot, no output leases or
// cross-queue readers. Intended CPU bound: one coordinator pending + one submitting
// owner (well below four leases); callers must not accumulate additional owners.
// Immutable configuration: destroy/drain the old instance before same-session
// replacement. stop is permanent, never frees resources still read by the GPU.
// 512 MiB session allocation accounting, not process RSS/opaque driver caches or
// a global overlap guarantee. Encoding/submission is NOT GPU correctness proof.
@interface MLPyrowaveMetalDecoder : NSObject
@property(nonatomic, readonly) MLPyrowaveConfiguration configuration;
// Fixed heaps plus conservative CPU reservation; excludes caller resources.
@property(nonatomic, readonly) uint64_t sessionAllocationBytes;
@property(nonatomic, readonly, nullable) NSError *lastError;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
// Uses supplied actual renderer device/queue only. Clock/context are borrowed,
// nonthrowing, same monotonic microsecond epoch as receive/deadline, and must
// outlive this decoder. Regression permanently stops admission.
+ (nullable instancetype)decoderWithKernels:(nullable MLPyrowaveMetalKernels *)kernels
                              commandQueue:(nullable id<MTLCommandQueue>)commandQueue
                             configuration:(MLPyrowaveConfiguration)configuration
                                     clock:(nullable MLPyrowaveClock)clock
                                   context:(nullable void *)context
                                     error:(NSError * _Nullable * _Nullable)error;
// Borrows the immutable validated CPU owner only through this synchronous call.
// Nonblocking trylock admission; backend alone commits exactly once on Submitted.
- (MLPyrowaveMetalSubmitResult)submitFrame:(nullable MLPyrowaveFrame *)frame
                            presentation:(nullable MLPyrowaveMetalPresentation)presentation
                              completion:(nullable MLPyrowaveMetalCompletion)completion;
- (void)stop;
@end
NS_ASSUME_NONNULL_END
#endif
