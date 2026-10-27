// SPDX-License-Identifier: GPL-3.0-only
#import "MLPyrowaveMetalRenderer.h"
#include "MLPyrowavePresentationBudget.h"
#include "MLPyrowaveCodec.hpp"
#include <os/lock.h>
#include <algorithm>
#include <cmath>

namespace {
NSString *const Shader = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Raster { float4 position [[position]]; float2 uv; };
struct Color { float yOffset, yScale, cScale, chromaOffset; };
vertex Raster pyrowaveVertex(uint index [[vertex_id]]) {
    float2 uv = float2((index << 1) & 2, index & 2);
    return {float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0), uv};
}
fragment float4 pyrowaveFragment(Raster in [[stage_in]],
    texture2d<float> y [[texture(0)]], texture2d<float> cb [[texture(1)]],
    texture2d<float> cr [[texture(2)]], sampler linearClamp [[sampler(0)]],
    constant Color &color [[buffer(0)]]) {
    float2 chromaUV = in.uv + float2(color.chromaOffset, 0.0);
    float Y = (y.sample(linearClamp, in.uv).r - color.yOffset) * color.yScale;
    float Cb = (cb.sample(linearClamp, chromaUV).r - 128.0 / 255.0) * color.cScale;
    float Cr = (cr.sample(linearClamp, chromaUV).r - 128.0 / 255.0) * color.cScale;
    return float4(Y + 1.5748 * Cr, Y - .187324 * Cb - .468124 * Cr,
                  Y + 1.8556 * Cb, 1.0);
}
)MSL";
struct Color { float yOffset, yScale, cScale, chromaOffset; };
static_assert(sizeof(Color) == 16, "Color ABI");
id Fail(NSError **error, NSString *message)
{
    if (error) *error = [NSError errorWithDomain:MLPyrowaveMetalErrorDomain
        code:MLPyrowaveMetalErrorPipeline userInfo:@{NSLocalizedDescriptionKey: message}];
    return nil;
}
bool ValidTarget(id<MTLTexture> t, id<MTLDevice> d)
{
    return t && t.device == d && t.storageMode != MTLStorageModeMemoryless &&
        t.pixelFormat == MTLPixelFormatBGRA8Unorm &&
        t.textureType == MTLTextureType2D && t.sampleCount == 1 &&
        t.mipmapLevelCount == 1 && t.arrayLength == 1 && t.depth == 1 &&
        t.width && t.height && (t.usage & MTLTextureUsageRenderTarget);
}
class Unlock final {
    os_unfair_lock *lock_;
public:
    explicit Unlock(os_unfair_lock *lock) : lock_(lock) {}
    ~Unlock() { os_unfair_lock_unlock(lock_); }
    Unlock(const Unlock &) = delete;
    Unlock &operator=(const Unlock &) = delete;
};
} // namespace

@interface MLPyrowaveMetalRenderer () {
    os_unfair_lock _lock;
    bool _stopped;
    MLPyrowaveCoordinator *_coordinator;
    MLPyrowaveMetalDecoder *_decoder;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLSamplerState> _sampler;
}
- (instancetype)initWithCoordinator:(MLPyrowaveCoordinator *)coordinator
    decoder:(MLPyrowaveMetalDecoder *)decoder queue:(id<MTLCommandQueue>)queue
    pipeline:(id<MTLRenderPipelineState>)pipeline sampler:(id<MTLSamplerState>)sampler;
- (MLPyrowaveMetalSubmitResult)renderTexture:(nullable id<MTLTexture>)texture
    drawable:(nullable id<CAMetalDrawable>)drawable
    completion:(nullable MLPyrowaveMetalCompletion)completion;
@end

@implementation MLPyrowaveMetalRenderer
- (NSError *)lastError { return _decoder.lastError; }
- (instancetype)initWithCoordinator:(MLPyrowaveCoordinator *)coordinator
    decoder:(MLPyrowaveMetalDecoder *)decoder queue:(id<MTLCommandQueue>)queue
    pipeline:(id<MTLRenderPipelineState>)pipeline sampler:(id<MTLSamplerState>)sampler
{
    self = [super init];
    if (self) {
        _lock = OS_UNFAIR_LOCK_INIT;
        _coordinator = coordinator; _decoder = decoder; _queue = queue;
        _pipeline = pipeline; _sampler = sampler; _configuration = decoder.configuration;
    }
    return self;
}
+ (instancetype)rendererWithCommandQueue:(id<MTLCommandQueue>)commandQueue
    configuration:(MLPyrowaveConfiguration)configuration clock:(MLPyrowaveClock)clock
    context:(void *)context error:(NSError **)error
{
    if (error) *error = nil;
    if (!clock || !configuration.generation || configuration.maxPacketSize < 129 ||
        configuration.maxPacketSize > 65536)
        return Fail(error, @"Renderer requires clock, nonzero generation and packet size 129..65536");
    MLPyrowave::CodecSession session;
    session.width = configuration.width; session.height = configuration.height;
    session.range = configuration.range; session.siting = configuration.siting;
    if (!MLPyrowave::IsSupportedCodecSession(session))
        return Fail(error, @"Renderer requires supported even SDR 709 8-bit 420 session/range/siting");
    id<MTLDevice> device = commandQueue.device;
    if (!commandQueue || !device) return Fail(error, @"Renderer requires actual queue/device");
    MLPyrowaveCoordinator *coordinator = [[MLPyrowaveCoordinator alloc]
        initWithConfiguration:configuration clock:clock context:context];
    if (!coordinator) return Fail(error, @"Renderer coordinator allocation failed");
    MLPyrowaveMetalKernels *kernels = [MLPyrowaveMetalKernels kernelsWithDevice:device error:error];
    if (!kernels) return nil;
    MLPyrowaveMetalDecoder *decoder = [MLPyrowaveMetalDecoder decoderWithKernels:kernels
        commandQueue:commandQueue configuration:configuration clock:clock context:context error:error];
    if (!decoder) return nil;
    NSError *compileError = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:Shader options:nil error:&compileError];
    id<MTLFunction> vertex = [library newFunctionWithName:@"pyrowaveVertex"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"pyrowaveFragment"];
    if (!library || !vertex || !fragment)
        return Fail(error, compileError.localizedDescription ?: @"Renderer shader functions unavailable");
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    if (!desc) return Fail(error, @"Renderer pipeline descriptor allocation failed");
    desc.vertexFunction = vertex; desc.fragmentFunction = fragment;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&compileError];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    if (!sd) return Fail(error, @"Renderer sampler descriptor allocation failed");
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    if (!pipeline || pipeline.device != device || !sampler || sampler.device != device ||
        commandQueue.device != device || kernels.device != device)
        return Fail(error, compileError.localizedDescription ?: @"Renderer pipeline/sampler device mismatch");
    MLPyrowaveMetalRenderer *renderer = [[self alloc] initWithCoordinator:coordinator decoder:decoder
        queue:commandQueue pipeline:pipeline sampler:sampler];
    if (!renderer) return Fail(error, @"Renderer object allocation failed");
    return renderer; // No partially constructed renderer escapes, no GPU waits.
}
- (BOOL)canPresentWidth:(NSUInteger)width height:(NSUInteger)height
{
    if (!width || !height || width > 16384 || height > 16384) return NO;
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:width height:height mipmapped:NO];
    if (!d) return NO;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    const MTLSizeAndAlign sa = [_queue.device heapTextureSizeAndAlignWithDescriptor:d];
    if (!sa.size || !sa.align || sa.align - 1 > UINT64_MAX - sa.size) return NO;
    return MLPyrowavePresentationFitsBudget(_decoder.sessionAllocationBytes,
        uint64_t(sa.size) + (sa.align - 1), 2);
}
- (int)submitPacket:(const LI_PYROWAVE_RECEIVED_PACKET *)packet
{
    return [_coordinator submitPacket:packet]; // Never acquires renderer/decoder lock.
}
- (MLPyrowaveMetalSubmitResult)renderPendingToDrawable:(id<CAMetalDrawable>)drawable
    completion:(MLPyrowaveMetalCompletion)completion
{
    return [self renderTexture:drawable.texture drawable:drawable completion:completion];
}
- (MLPyrowaveMetalSubmitResult)renderPendingToTexture:(id<MTLTexture>)texture
    completion:(MLPyrowaveMetalCompletion)completion
{
    return [self renderTexture:texture drawable:nil completion:completion];
}
- (MLPyrowaveMetalSubmitResult)renderTexture:(id<MTLTexture>)texture
    drawable:(id<CAMetalDrawable>)drawable completion:(MLPyrowaveMetalCompletion)completion
{
    @autoreleasepool {
        if (!os_unfair_lock_trylock(&_lock)) return MLPyrowaveMetalSubmitBusy;
        Unlock unlock(&_lock);
        if (_stopped) return MLPyrowaveMetalSubmitStopped;
        if (!ValidTarget(texture, _queue.device)) return MLPyrowaveMetalSubmitDropped;
        if (!MLPyrowavePresentationFitsBudget(_decoder.sessionAllocationBytes,
            texture.allocatedSize, 2)) return MLPyrowaveMetalSubmitDropped;
        const auto c = _configuration;
        const double scale = std::min(double(texture.width) / c.width, double(texture.height) / c.height);
        const double w = c.width * scale, h = c.height * scale;
        const MTLViewport viewport = {(texture.width - w) * .5, (texture.height - h) * .5, w, h, 0, 1};
        const NSUInteger left = std::ceil(std::max(0.0, viewport.originX));
        const NSUInteger top = std::ceil(std::max(0.0, viewport.originY));
        const NSUInteger right = std::floor(std::min(double(texture.width), viewport.originX + w));
        const NSUInteger bottom = std::floor(std::min(double(texture.height), viewport.originY + h));
        if (right <= left || bottom <= top) return MLPyrowaveMetalSubmitDropped;
        const MTLScissorRect scissor = {left, top, right - left, bottom - top};
        MLPyrowaveFrame *frame = [_coordinator tryTakeFrame];
        if (!frame) return MLPyrowaveMetalSubmitDropped;
        // Synchronous borrow captures resources/drawable only, never renderer/UI owners.
        id<MTLRenderPipelineState> pipeline = _pipeline;
        id<MTLSamplerState> sampler = _sampler;
        id<MTLCommandQueue> queue = _queue;
        const Color color = {c.range ? 16.f / 255.f : 0.f, c.range ? 255.f / 219.f : 1.f,
            c.range ? 255.f / 224.f : 1.f, c.siting ? .5f / c.width : 0.f};
        MLPyrowaveMetalPresentation presentation = ^BOOL(id<MTLCommandBuffer> cmd,
            id<MTLTexture> y, id<MTLTexture> cb, id<MTLTexture> cr) {
            if (cmd.commandQueue != queue || y.device != texture.device ||
                cb.device != texture.device || cr.device != texture.device) return NO;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            if (!pass) return NO;
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> encoder = [cmd renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return NO;
            [encoder setRenderPipelineState:pipeline];
            [encoder setViewport:viewport];
            [encoder setScissorRect:scissor];
            [encoder setFragmentTexture:y atIndex:0];
            [encoder setFragmentTexture:cb atIndex:1];
            [encoder setFragmentTexture:cr atIndex:2];
            [encoder setFragmentSamplerState:sampler atIndex:0];
            [encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            if (drawable) [cmd presentDrawable:drawable];
            return YES; // Decoder alone commits after all final readers/presentation.
        };
        return [_decoder submitFrame:frame presentation:presentation completion:completion];
    }
}
- (void)stop
{
    os_unfair_lock_lock(&_lock);
    Unlock unlock(&_lock);
    _stopped = true;
    // Lock order: renderer -> coordinator OR decoder; neither calls renderer.
    [_coordinator stop];
    [_decoder stop];
}
@end
