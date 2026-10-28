// SPDX-License-Identifier: GPL-3.0-only
#import "MLPyrowaveMetalKernels.h"
#include "MLPyrowaveMetalShaders.h"
#include "MLPyrowaveMetalLibrary.h"

NSErrorDomain const MLPyrowaveMetalErrorDomain = @"MLPyrowaveMetalErrorDomain";

static id Fail(NSError **error, MLPyrowaveMetalError code, NSString *message,
               NSError *cause = nil)
{
    if (error) *error = [NSError errorWithDomain:MLPyrowaveMetalErrorDomain code:code
        userInfo:cause ? @{NSLocalizedDescriptionKey: message, NSUnderlyingErrorKey: cause}
                       : @{NSLocalizedDescriptionKey: message}];
    return nil;
}

static id<MTLComputePipelineState> Pipeline(id<MTLDevice> device, id<MTLLibrary> library,
    NSString *name, MTLFunctionConstantValues *constants, NSUInteger threads,
    bool requireSIMD32, NSError **error)
{
    NSError *cause = nil;
    id<MTLFunction> function = constants
        ? [library newFunctionWithName:name constantValues:constants error:&cause]
        : [library newFunctionWithName:name];
    if (!function) return Fail(error, MLPyrowaveMetalErrorPipeline, name, cause);
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    if (!descriptor) return Fail(error, MLPyrowaveMetalErrorPipeline, name);
    descriptor.computeFunction = function;
    descriptor.maxTotalThreadsPerThreadgroup = threads;
    descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = requireSIMD32;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone
                                          reflection:nil error:&cause];
    if (!pipeline) return Fail(error, MLPyrowaveMetalErrorPipeline, name, cause);
    if (pipeline.device != device || pipeline.maxTotalThreadsPerThreadgroup < threads ||
        (requireSIMD32 && pipeline.threadExecutionWidth != 32) ||
        pipeline.staticThreadgroupMemoryLength > device.maxThreadgroupMemoryLength)
        return Fail(error, MLPyrowaveMetalErrorPipeline, @"Pipeline device/limits mismatch");
    return pipeline;
}

@interface MLPyrowaveMetalKernels ()
+ (instancetype)uncachedKernelsWithDevice:(id<MTLDevice>)device error:(NSError **)error;
- (instancetype)initWithDevice:(id<MTLDevice>)device
                        dequant:(id<MTLComputePipelineState>)dequant
                           idwt:(id<MTLComputePipelineState>)idwt
                      idwtFinal:(id<MTLComputePipelineState>)idwtFinal
                        sampler:(id<MTLSamplerState>)sampler;
@end

@implementation MLPyrowaveMetalKernels
- (instancetype)initWithDevice:(id<MTLDevice>)device
                        dequant:(id<MTLComputePipelineState>)dequant
                           idwt:(id<MTLComputePipelineState>)idwt
                      idwtFinal:(id<MTLComputePipelineState>)idwtFinal
                        sampler:(id<MTLSamplerState>)sampler
{
    self = [super init];
    if (self) {
        _device = device;
        _dequant = dequant;
        _idwt = idwt;
        _idwtFinal = idwtFinal;
        _sampler = sampler;
    }
    return self;
}

+ (instancetype)kernelsWithDevice:(id<MTLDevice>)device error:(NSError **)error
{
    @synchronized ([MLPyrowaveMetalKernels class]) {
        static __strong MLPyrowaveMetalKernels *cached = nil;
        if (error) *error = nil;
        if (!device) return Fail(error, MLPyrowaveMetalErrorUnsupportedDevice, @"No Metal device");
        if (cached.device == device) return cached;
        MLPyrowaveMetalKernels *kernels = [self uncachedKernelsWithDevice:device error:error];
        if (kernels) cached = kernels; // Publish only a complete successful bundle.
        return kernels;
    }
}

+ (instancetype)uncachedKernelsWithDevice:(id<MTLDevice>)device error:(NSError **)error
{
    if (error) *error = nil;
    if (!device) return Fail(error, MLPyrowaveMetalErrorUnsupportedDevice, @"No Metal device");
    if (@available(macOS 13.0, *)) {
        if (![device supportsFamily:MTLGPUFamilyApple7] || device.maxThreadsPerThreadgroup.width < 128)
            return Fail(error, MLPyrowaveMetalErrorUnsupportedDevice, @"Requires Apple7 and 128 threads");
        MTLCompileOptions *options = [MTLCompileOptions new];
        if (!options)
            return Fail(error, MLPyrowaveMetalErrorPipeline, @"Shader options allocation failed");
        options.languageVersion = MTLLanguageVersion3_0;
        // Explicit legacy precision switch, including SDKs that deprecate it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        NSError *cause = nil;
        NSBundle *bundle = [NSBundle bundleForClass:MLPyrowaveMetalKernels.class];
        // Independent libraries: generated helper names collide if concatenated.
        id<MTLLibrary> dequantLibrary = MLPyrowaveLoadLibrary(device, bundle,
            @"pyrowave-dequant", wavelet_dequant_msl_source, options, &cause);
        if (!dequantLibrary) return Fail(error, MLPyrowaveMetalErrorPipeline, @"Dequant library failed", cause);
        cause = nil;
        id<MTLLibrary> idwtLibrary = MLPyrowaveLoadLibrary(device, bundle,
            @"pyrowave-idwt", idwt_msl_source, options, &cause);
        if (!idwtLibrary) return Fail(error, MLPyrowaveMetalErrorPipeline, @"IDWT library failed", cause);
        id<MTLComputePipelineState> dequant = Pipeline(device, dequantLibrary,
            @"pyrowave_wavelet_dequant", nil, 128, true, error);
        if (!dequant) return nil;
        MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
        if (!constants) return Fail(error, MLPyrowaveMetalErrorPipeline, @"Function constants allocation failed");
        bool final = false;
        [constants setConstantValue:&final type:MTLDataTypeBool atIndex:0];
        id<MTLComputePipelineState> idwt = Pipeline(device, idwtLibrary,
            @"pyrowave_idwt", constants, 64, false, error);
        if (!idwt) return nil;
        final = true;
        [constants setConstantValue:&final type:MTLDataTypeBool atIndex:0];
        id<MTLComputePipelineState> idwtFinal = Pipeline(device, idwtLibrary,
            @"pyrowave_idwt", constants, 64, false, error);
        if (!idwtFinal) return nil;
        MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
        if (!descriptor) return Fail(error, MLPyrowaveMetalErrorPipeline, @"Sampler descriptor allocation failed");
        descriptor.minFilter = MTLSamplerMinMagFilterNearest;
        descriptor.magFilter = MTLSamplerMinMagFilterNearest;
        descriptor.mipFilter = MTLSamplerMipFilterNearest;
        descriptor.sAddressMode = MTLSamplerAddressModeMirrorRepeat;
        descriptor.tAddressMode = MTLSamplerAddressModeMirrorRepeat;
        descriptor.rAddressMode = MTLSamplerAddressModeMirrorRepeat;
        descriptor.normalizedCoordinates = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
        if (!sampler || sampler.device != device)
            return Fail(error, MLPyrowaveMetalErrorPipeline, @"Sampler creation/device failed");
        MLPyrowaveMetalKernels *kernels = [[self alloc] initWithDevice:device dequant:dequant
            idwt:idwt idwtFinal:idwtFinal sampler:sampler];
        if (!kernels) return Fail(error, MLPyrowaveMetalErrorPipeline, @"Bundle allocation failed");
        return kernels;
    }
    return Fail(error, MLPyrowaveMetalErrorUnsupportedDevice, @"Requires macOS 13 and MSL 3.0");
}
@end
