// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_METAL_KERNELS_H
#define ML_PYROWAVE_METAL_KERNELS_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN
FOUNDATION_EXPORT NSErrorDomain const MLPyrowaveMetalErrorDomain;
typedef NS_ERROR_ENUM(MLPyrowaveMetalErrorDomain, MLPyrowaveMetalError) {
    MLPyrowaveMetalErrorUnsupportedDevice = 1,
    MLPyrowaveMetalErrorPipeline = 2,
};

// Pipeline capability bundle ONLY, for the supplied actual renderer device.
// Not format/dimensions/budget/GPU correctness or final feature preflight.
// Does not create a default device, allocate decode resources or dispatch work.
@interface MLPyrowaveMetalKernels : NSObject
@property(nonatomic, readonly, strong) id<MTLDevice> device;
@property(nonatomic, readonly, strong) id<MTLComputePipelineState> dequant;
@property(nonatomic, readonly, strong) id<MTLComputePipelineState> idwt;
@property(nonatomic, readonly, strong) id<MTLComputePipelineState> idwtFinal;
@property(nonatomic, readonly, strong) id<MTLSamplerState> sampler;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
+ (nullable instancetype)kernelsWithDevice:(nullable id<MTLDevice>)device
                                    error:(NSError * _Nullable * _Nullable)error;
@end
NS_ASSUME_NONNULL_END
#endif
