// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_METAL_RENDERER_H
#define ML_PYROWAVE_METAL_RENDERER_H

#import "MLPyrowaveMetalDecoder.h"
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN
// Standalone opt-in peer; no Connection/settings/live-stream wiring.
// Fixed session, one coordinator pending owner + one synchronous render-local
// owner. A consumed frame is dropped on decoder Busy; no retry/extra frame queue.
// Render is nonblocking trylock; stop is permanent and never waits for the GPU.
// Clock/context are borrowed, nonthrowing, thread-safe/nonreentrant and must
// outlive this instance (same monotonic microsecond epoch as packet receipt).
@interface MLPyrowaveMetalRenderer : NSObject
@property(nonatomic, readonly) MLPyrowaveConfiguration configuration;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
// Uses ONLY the supplied existing renderer queue/device, with exact identity.
// CPU config validation precedes pipeline compilation; no GPU waits.
+ (nullable instancetype)rendererWithCommandQueue:(nullable id<MTLCommandQueue>)commandQueue
                                   configuration:(MLPyrowaveConfiguration)configuration
                                           clock:(nullable MLPyrowaveClock)clock
                                         context:(nullable void *)context
                                           error:(NSError * _Nullable * _Nullable)error;
// Synchronous borrowed public Common descriptor; returns Common CPU ack ONLY,
// not GPU/presentation success. Independent coordinator thread-safety.
- (int)submitPacket:(nullable const LI_PYROWAVE_RECEIVED_PACKET *)packet;
// Nil/invalid target or empty inward-rounded fit scissor preserves pending.
// Requires same device, BGRA8Unorm, non-memoryless storage,
// 2D/sample1/mip1/array1/RenderTarget. Black-clear aspect fit, encoded Rec.709
// RGB, NO sRGB/gamma conversion. Caller layer MUST use kCGColorSpaceITUR_709,
// with SDR/EDR off. Completion is forwarded directly to the decoder; no UI or
// renderer captures. Only Submitted completes; completion is not display ack.
- (MLPyrowaveMetalSubmitResult)renderPendingToDrawable:(nullable id<CAMetalDrawable>)drawable
                                          completion:(nullable MLPyrowaveMetalCompletion)completion;
// Same pass without presentDrawable. Caller owns target and MUST NOT reuse/read
// until completion; normal command-buffer references retain GPU resources.
// Caller must account external targets within the existing 256 MiB session
// budget; decoder accounting alone does not cover them or opaque driver caches.
- (MLPyrowaveMetalSubmitResult)renderPendingToTexture:(nullable id<MTLTexture>)texture
                                         completion:(nullable MLPyrowaveMetalCompletion)completion;
- (void)stop;
@end
NS_ASSUME_NONNULL_END
#endif
