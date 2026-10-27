// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT AND GPL-3.0-only
// Host dispatch/view algorithms adapted from the pin in pyrowave-metal-provenance.md.
#import "MLPyrowaveMetalDecoder.h"
#include "MLPyrowavePresentationBudget.h"
#include "MLPyrowaveReceiver.hpp"
#include <os/lock.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr uint64_t MiB = 1024 * 1024, Payload = 16 * MiB + 16;
constexpr MTLTextureUsage ReadWrite = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
constexpr MTLResourceOptions Shared = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked;
struct Layout {
    uint32_t width[5] = {}, height[5] = {};
    uint32_t offset[3][5][4] = {}, stride[3][5][4] = {}, count = 0;
    uint64_t cpuReserve = 0, offsetBytes = 0;
};
struct DequantPush {
    int32_t resolution[2], outputLayer, blockOffset, blockStride, pad;
};
struct IdwtPush { int32_t resolution[2]; float inv[2]; };
static_assert(sizeof(DequantPush) == 24, "Dequant ABI");
static_assert(sizeof(IdwtPush) == 16, "IDWT ABI");
bool Add(uint64_t a, uint64_t b, uint64_t &out)
{
    if (b > UINT64_MAX - a) return false;
    out = a + b;
    return true;
}
bool Mul(uint64_t a, uint64_t b, uint64_t &out)
{
    if (b && a > UINT64_MAX / b) return false;
    out = a * b;
    return true;
}
bool LayoutBudget(const MLPyrowave::CodecSession &s, uint64_t maxBuffer, Layout &l)
{
    // Supported-session check precedes this bounded alignment arithmetic.
    const uint64_t aw = std::max<uint64_t>(128, (uint64_t(s.width) + 31) & ~uint64_t(31));
    const uint64_t ah = std::max<uint64_t>(128, (uint64_t(s.height) + 31) & ~uint64_t(31));
    uint64_t count = 0;
    for (int level = 4; level >= 0; --level) {
        l.width[level] = uint32_t(aw >> (level + 1));
        l.height[level] = uint32_t(ah >> (level + 1));
        const uint64_t stride = (l.width[level] + 31) / 32;
        uint64_t blocks;
        if (!Mul(stride, (l.height[level] + 31) / 32, blocks)) return false;
        for (unsigned c = 0; c < 3; ++c) {
            if (level == 0 && c != 0) continue;
            for (unsigned b = level == 4 ? 0 : 1; b < 4; ++b) {
                if (count > UINT32_MAX || stride > UINT32_MAX) return false;
                l.offset[c][level][b] = uint32_t(count);
                l.stride[c][level][b] = uint32_t(stride);
                if (!Add(count, blocks, count)) return false;
            }
        }
    }
    uint64_t owner, owners;
    if (!count || count > UINT32_MAX || !Mul(count, 4, l.offsetBytes) ||
        l.offsetBytes > maxBuffer || Payload > maxBuffer ||
        !Add(Payload, l.offsetBytes, owner) || !Add(owner, 4096, owner) ||
        !Mul(owner, 2, owners) || !Add(128 * MiB + 32 * MiB + MiB, owners, l.cpuReserve))
        return false;
    l.count = uint32_t(count);
    return l.cpuReserve < MLPyrowaveSessionAllocationCap && l.offsetBytes <= std::numeric_limits<NSUInteger>::max();
}
id Fail(NSError **error, NSString *message)
{
    if (error) *error = [NSError errorWithDomain:MLPyrowaveMetalErrorDomain
        code:MLPyrowaveMetalErrorPipeline userInfo:@{NSLocalizedDescriptionKey: message}];
    return nil;
}
bool PaddedSize(MTLSizeAndAlign sa, uint64_t &total)
{
    uint64_t padded;
    return sa.size && sa.align && Add(sa.size, sa.align - 1, padded) && Add(total, padded, total);
}
MTLTextureDescriptor *TextureDescriptor(NSUInteger w, NSUInteger h, bool pyramid)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    if (!d) return nil;
    d.textureType = pyramid ? MTLTextureType2DArray : MTLTextureType2D;
    d.pixelFormat = pyramid ? MTLPixelFormatR32Float : MTLPixelFormatR8Unorm;
    d.width = w; d.height = h;
    d.arrayLength = pyramid ? 12 : 1;
    d.mipmapLevelCount = pyramid ? 5 : 1;
    d.storageMode = MTLStorageModePrivate;
    d.hazardTrackingMode = MTLHazardTrackingModeTracked;
    d.usage = ReadWrite | (pyramid ? MTLTextureUsagePixelFormatView : 0);
    return d;
}
bool TextureMatches(id<MTLTexture> t, id<MTLDevice> device, MTLTextureType type,
    MTLPixelFormat format, NSUInteger w, NSUInteger h, NSUInteger slices,
    NSUInteger mips, MTLTextureUsage usage)
{
    return t && t.device == device && t.textureType == type && t.pixelFormat == format &&
        t.width == w && t.height == h && t.depth == 1 && t.arrayLength == slices &&
        t.mipmapLevelCount == mips && t.sampleCount == 1 &&
        t.storageMode == MTLStorageModePrivate && (t.usage & usage) == usage;
}
bool PipelinesMatch(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q)
{
    id<MTLDevice> d = k.device;
    if (!d || !q || q.device != d || !k.sampler || k.sampler.device != d ||
        d.maxThreadsPerThreadgroup.width < 128 || k.dequant.threadExecutionWidth != 32) return false;
    id<MTLComputePipelineState> pipelines[] = {k.dequant, k.idwt, k.idwtFinal};
    for (unsigned i = 0; i < 3; ++i)
        if (!pipelines[i] || pipelines[i].device != d ||
            pipelines[i].maxTotalThreadsPerThreadgroup < (i ? 64u : 128u) ||
            pipelines[i].staticThreadgroupMemoryLength > d.maxThreadgroupMemoryLength) return false;
    return true;
}
bool SessionMatches(const MLPyrowave::CodecSession &a, const MLPyrowave::CodecSession &b)
{
    return a.width == b.width && a.height == b.height && a.bitDepth == b.bitDepth &&
        a.chroma == b.chroma && a.primaries == b.primaries && a.transfer == b.transfer &&
        a.transform == b.transform && a.range == b.range && a.siting == b.siting;
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

// Private retirement owner: resources immutable after factory publication; only
// lock/admission/clock fields mutate, under lock. Never retains decoder or frame.
@interface MLPyrowaveMetalPool : NSObject {
@public
    Layout layout;
    MLPyrowaveConfiguration configuration;
    MLPyrowave::CodecSession session;
    MLPyrowaveMetalKernels *kernels;
    id<MTLCommandQueue> queue;
    id<MTLHeap> privateHeap, sharedHeap;
    id<MTLBuffer> payload, offsets;
    id<MTLTexture> pyramid, layers[3][5], ll[3][5], planes[3];
    uint64_t gpuBytes;
    os_unfair_lock lock;
    bool stopped, busy, sampled, regressed;
    NSError *lastError;
    MLPyrowaveClock clock;
    void *context;
    uint64_t lastClock;
}
@end
@implementation MLPyrowaveMetalPool
@end

namespace {
bool AllocateViews(MLPyrowaveMetalPool *p, NSError **error)
{
    id<MTLDevice> d = p->kernels.device;
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned l = 0; l < 5; ++l) {
            p->layers[c][l] = [p->pyramid newTextureViewWithPixelFormat:MTLPixelFormatR32Float
                textureType:MTLTextureType2DArray levels:NSMakeRange(l, 1) slices:NSMakeRange(4 * c, 4)];
            p->ll[c][l] = [p->pyramid newTextureViewWithPixelFormat:MTLPixelFormatR32Float
                textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(4 * c, 1)];
            if (!TextureMatches(p->layers[c][l], d, MTLTextureType2DArray, MTLPixelFormatR32Float,
                    p->layout.width[l], p->layout.height[l], 4, 1, ReadWrite) ||
                !TextureMatches(p->ll[c][l], d, MTLTextureType2D, MTLPixelFormatR32Float,
                    p->layout.width[l], p->layout.height[l], 1, 1, ReadWrite)) {
                Fail(error, [NSString stringWithFormat:@"Decoder R32Float view allocation/validation failed at component %u mip %u", c, l]);
                return false;
            }
        }
    }
    return true;
}
bool AllocateResources(MLPyrowaveMetalPool *p, MTLTextureDescriptor * __strong const desc[4], NSError **error)
{
    id<MTLDevice> d = p->kernels.device;
    p->pyramid = [p->privateHeap newTextureWithDescriptor:desc[0]];
    if (!TextureMatches(p->pyramid, d, MTLTextureType2DArray, MTLPixelFormatR32Float,
        p->layout.width[0], p->layout.height[0], 12, 5,
        ReadWrite | MTLTextureUsagePixelFormatView)) {
        Fail(error, @"Decoder private R32Float 12-slice five-mip pyramid allocation/validation failed");
        return false;
    }
    for (unsigned c = 0; c < 3; ++c) {
        p->planes[c] = [p->privateHeap newTextureWithDescriptor:desc[c + 1]];
        if (!TextureMatches(p->planes[c], d, MTLTextureType2D, MTLPixelFormatR8Unorm,
            desc[c + 1].width, desc[c + 1].height, 1, 1, ReadWrite)) {
            Fail(error, [NSString stringWithFormat:@"Decoder private R8Unorm plane %u allocation/validation failed", c]);
            return false;
        }
    }
    p->payload = [p->sharedHeap newBufferWithLength:Payload options:Shared];
    p->offsets = [p->sharedHeap newBufferWithLength:p->layout.offsetBytes options:Shared];
    id<MTLBuffer> buffers[] = {p->payload, p->offsets};
    for (unsigned i = 0; i < 2; ++i)
        if (!buffers[i] || buffers[i].device != d || !buffers[i].contents ||
            buffers[i].storageMode != MTLStorageModeShared ||
            buffers[i].length != (i ? p->layout.offsetBytes : Payload)) {
            Fail(error, i ? @"Decoder shared offset buffer allocation/device/length/contents failed"
                          : @"Decoder shared payload buffer allocation/device/length/contents failed");
            return false;
        }
    std::memset(p->payload.contents, 0, Payload);
    std::memset(p->offsets.contents, 0xff, p->layout.offsetBytes);
    return AllocateViews(p, error);
}
bool AllocateHeaps(MLPyrowaveMetalPool *p, MTLTextureDescriptor * __strong const desc[4], NSError **error)
{
    id<MTLDevice> d = p->kernels.device;
    uint64_t privateSize = 0, sharedSize = 0, sum;
    for (unsigned i = 0; i < 4; ++i)
        if (!desc[i] || !PaddedSize([d heapTextureSizeAndAlignWithDescriptor:desc[i]], privateSize)) {
            Fail(error, @"Decoder texture descriptor or heap texture size/alignment query invalid/overflowed");
            return false;
        }
    if (!PaddedSize([d heapBufferSizeAndAlignWithLength:Payload options:Shared], sharedSize) ||
        !PaddedSize([d heapBufferSizeAndAlignWithLength:p->layout.offsetBytes options:Shared], sharedSize) ||
        !Add(privateSize, sharedSize, sum) || !Add(sum, p->layout.cpuReserve, sum) || sum > MLPyrowaveSessionAllocationCap) {
        Fail(error, @"Decoder buffer heap query invalid/overflowed, or CPU reservation plus padded heaps exceeds 512 MiB");
        return false; // BEFORE either heap is created; no growing upload storage.
    }
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    if (!hd) { Fail(error, @"Decoder heap descriptor allocation failed"); return false; }
    hd.hazardTrackingMode = MTLHazardTrackingModeTracked;
    hd.storageMode = MTLStorageModePrivate; hd.size = privateSize;
    p->privateHeap = [d newHeapWithDescriptor:hd];
    hd.storageMode = MTLStorageModeShared; hd.size = sharedSize;
    p->sharedHeap = [d newHeapWithDescriptor:hd];
    if (!p->privateHeap || !p->sharedHeap || p->privateHeap.device != d || p->sharedHeap.device != d ||
        p->privateHeap.storageMode != MTLStorageModePrivate || p->sharedHeap.storageMode != MTLStorageModeShared ||
        p->privateHeap.size < privateSize || p->sharedHeap.size < sharedSize ||
        !Add(p->privateHeap.size, p->sharedHeap.size, p->gpuBytes) ||
        !Add(p->gpuBytes, p->layout.cpuReserve, sum) || sum > MLPyrowaveSessionAllocationCap) {
        Fail(error, @"Decoder fixed heaps allocation/device/storage/size validation failed or actual heaps plus CPU reservation exceeds 512 MiB");
        return false;
    }
    return AllocateResources(p, desc, error); // Views alias the pyramid; no double charge.
}
bool Timely(MLPyrowaveMetalPool *p, const MLPyrowaveFrameInfo &info)
{
    const uint64_t now = p->clock(p->context);
    if (p->sampled && now < p->lastClock) p->regressed = true;
    p->sampled = true; p->lastClock = now;
    if (p->regressed) p->stopped = true;
    return !p->stopped && now >= info.firstReceiveUs && now < info.deadlineUs;
}
bool ValidInput(MLPyrowaveMetalPool *p, MLPyrowaveFrame *frame)
{
    if (frame.generation != p->configuration.generation) return false;
    const auto *received = [frame validatedFrame];
    if (!received || !received->codec) return false;
    const auto &f = *received->codec;
    if (!SessionMatches(f.session(), p->session) || f.offsets().size() != p->layout.count ||
        f.payloadSize() < 8 || f.payloadSize() > 16 * MiB ||
        f.bytes().size() != f.payloadSize() + 16) return false;
    for (uint32_t offset : f.offsets())
        if (offset != UINT32_MAX && ((offset & 3) || uint64_t(offset) + 8 > f.payloadSize())) return false;
    return true;
}
bool EncodeDequant(MLPyrowaveMetalPool *p, id<MTLCommandBuffer> cmd)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    if (!enc) return false;
    enc.label = @"PyroWave dequant";
    [enc setComputePipelineState:p->kernels.dequant];
    [enc setBuffer:p->payload offset:0 atIndex:0];
    [enc setBuffer:p->offsets offset:0 atIndex:2];
    for (unsigned l = 0; l < 5; ++l) {
        for (unsigned c = 0; c < 3; ++c) {
            if (l == 0 && c != 0) continue;
            [enc setTexture:p->layers[c][l] atIndex:0];
            for (unsigned b = l == 4 ? 0 : 1; b < 4; ++b) {
                DequantPush push = {{int32_t(p->layout.width[l]), int32_t(p->layout.height[l])},
                    int32_t(b), int32_t(p->layout.offset[c][l][b]), int32_t(p->layout.stride[c][l][b]), 0};
                [enc setBytes:&push length:sizeof(push) atIndex:1];
                [enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 31) / 32,
                    (push.resolution[1] + 31) / 32, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
            }
        }
    } // All bands dispatched: UINT32_MAX offsets explicitly write sparse zeros.
    [enc endEncoding];
    return true;
}
bool EncodeIDWT(MLPyrowaveMetalPool *p, id<MTLCommandBuffer> cmd)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    if (!enc) return false;
    enc.label = @"PyroWave IDWT";
    [enc setSamplerState:p->kernels.sampler atIndex:0];
    id<MTLComputePipelineState> currentPipeline = nil;
    for (int l = 4; l >= 0; --l) {
        if (l != 4) [enc memoryBarrierWithScope:MTLBarrierScopeTextures];
        IdwtPush push = {{int32_t(p->layout.height[l]), int32_t(p->layout.width[l])},
            {1.0f / p->layout.height[l], 1.0f / p->layout.width[l]}};
        [enc setBytes:&push length:sizeof(push) atIndex:0];
        for (unsigned c = 0; c < (l == 0 ? 1u : 3u); ++c) {
            const bool final = l == 0 || (l == 1 && c != 0);
            id<MTLTexture> output = final ? p->planes[c] : p->ll[c][l - 1];
            id<MTLComputePipelineState> nextPipeline = final ? p->kernels.idwtFinal : p->kernels.idwt;
            if (nextPipeline != currentPipeline) {
                [enc setComputePipelineState:nextPipeline];
                currentPipeline = nextPipeline;
            }
            [enc setTexture:p->layers[c][l] atIndex:0];
            [enc setTexture:output atIndex:1];
            [enc dispatchThreadgroups:MTLSizeMake((push.resolution[0] + 15) / 16,
                (push.resolution[1] + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
    }
    [enc endEncoding];
    return true;
}
} // namespace

@interface MLPyrowaveMetalDecoder () {
    MLPyrowaveMetalPool *_pool;
}
- (instancetype)initWithPool:(MLPyrowaveMetalPool *)pool;
@end
@implementation MLPyrowaveMetalDecoder
- (instancetype)initWithPool:(MLPyrowaveMetalPool *)pool
{
    self = [super init];
    if (self) { _pool = pool; _configuration = pool->configuration; }
    return self;
}
- (uint64_t)sessionAllocationBytes { return _pool->gpuBytes + _pool->layout.cpuReserve; }
- (NSError *)lastError
{
    os_unfair_lock_lock(&_pool->lock);
    NSError *snapshot = _pool->lastError;
    os_unfair_lock_unlock(&_pool->lock);
    return snapshot;
}
+ (instancetype)decoderWithKernels:(MLPyrowaveMetalKernels *)kernels
                       commandQueue:(id<MTLCommandQueue>)commandQueue
                      configuration:(MLPyrowaveConfiguration)configuration
                              clock:(MLPyrowaveClock)clock context:(void *)context error:(NSError **)error
{
    if (error) *error = nil;
    if (!clock || !configuration.generation || configuration.maxPacketSize < 129 ||
        configuration.maxPacketSize > 65536)
        return Fail(error, @"Decoder requires clock, nonzero generation and packet size 129..65536");
    MLPyrowave::CodecSession session;
    session.width = configuration.width; session.height = configuration.height;
    session.range = configuration.range; session.siting = configuration.siting;
    if (!MLPyrowave::IsSupportedCodecSession(session))
        return Fail(error, @"Decoder requires supported even SDR 709 8-bit 420 session/range/siting");
    if (!PipelinesMatch(kernels, commandQueue))
        return Fail(error, @"Decoder pipeline/sampler limits or actual renderer queue/device mismatch");
    Layout layout;
    if (!LayoutBudget(session, kernels.device.maxBufferLength, layout))
        return Fail(error, @"Decoder layout/CPU reservation exceeds 512 MiB or buffer/index limits");
    MLPyrowaveMetalPool *pool = [MLPyrowaveMetalPool new];
    if (!pool) return Fail(error, @"Decoder retirement pool allocation failed");
    pool->layout = layout; pool->session = session; pool->configuration = configuration;
    pool->kernels = kernels; pool->queue = commandQueue;
    pool->lock = OS_UNFAIR_LOCK_INIT; pool->clock = clock; pool->context = context;
    MTLTextureDescriptor *desc[4] = {TextureDescriptor(layout.width[0], layout.height[0], true),
        TextureDescriptor(session.width, session.height, false),
        TextureDescriptor(session.width / 2, session.height / 2, false),
        TextureDescriptor(session.width / 2, session.height / 2, false)};
    if (!AllocateHeaps(pool, desc, error)) return nil;
    MLPyrowaveMetalDecoder *decoder = [[self alloc] initWithPool:pool];
    if (!decoder) return Fail(error, @"Decoder object allocation failed");
    return decoder;
}
- (MLPyrowaveMetalSubmitResult)submitFrame:(MLPyrowaveFrame *)frame
                            presentation:(MLPyrowaveMetalPresentation)presentation
                              completion:(MLPyrowaveMetalCompletion)completion
{
    @autoreleasepool {
        MLPyrowaveMetalPool *p = _pool;
        if (!os_unfair_lock_trylock(&p->lock)) return MLPyrowaveMetalSubmitBusy;
        Unlock unlock(&p->lock);
        if (p->stopped) return MLPyrowaveMetalSubmitStopped;
        if (p->busy) return MLPyrowaveMetalSubmitBusy;
        if (!frame || !presentation || !ValidInput(p, frame)) return MLPyrowaveMetalSubmitDropped;
        const auto &f = *[frame validatedFrame]->codec;
        if (!Timely(p, f.frameInfo()))
            return p->stopped ? MLPyrowaveMetalSubmitStopped : MLPyrowaveMetalSubmitDropped;
        p->busy = true;
        id<MTLCommandBuffer> cmd = [p->queue commandBuffer];
        if (!cmd || cmd.commandQueue != p->queue) {
            p->busy = false; p->stopped = true;
            return MLPyrowaveMetalSubmitFailed;
        }
        std::memcpy(p->payload.contents, f.bytes().data(), f.bytes().size());
        cmd.label = @"PyroWave decode and present";
        std::memset(static_cast<uint8_t *>(p->payload.contents) + f.payloadSize(), 0, 16);
        auto *words = static_cast<uint32_t *>(p->offsets.contents);
        for (size_t i = 0; i < f.offsets().size(); ++i)
            words[i] = f.offsets()[i] == UINT32_MAX ? UINT32_MAX : f.offsets()[i] / 4;
        if (!EncodeDequant(p, cmd) || !EncodeIDWT(p, cmd)) {
            p->busy = false; p->stopped = true;
            return MLPyrowaveMetalSubmitFailed;
        }
        if (!presentation(cmd, p->planes[0], p->planes[1], p->planes[2])) {
            p->busy = false;
            return MLPyrowaveMetalSubmitDropped; // Uncommitted; no readers can escape borrow.
        }
        // Handler captures ONLY private retirement state and the supplied callback.
        MLPyrowaveMetalCompletion callback = [completion copy];
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            const BOOL success = completed.status == MTLCommandBufferStatusCompleted;
            os_unfair_lock_lock(&p->lock);
            p->busy = false;
            if (!success) {
                p->lastError = completed.error ?: [NSError errorWithDomain:MLPyrowaveMetalErrorDomain
                    code:completed.status userInfo:@{NSLocalizedDescriptionKey: @"PyroWave GPU command failed without a Metal error"}];
                p->stopped = true;
            }
            os_unfair_lock_unlock(&p->lock);
            if (callback) callback(success);
        }];
        if (!Timely(p, f.frameInfo())) { // Includes encoding, borrow and callback allocation time.
            p->busy = false;
            return p->stopped ? MLPyrowaveMetalSubmitStopped : MLPyrowaveMetalSubmitDropped;
        }
        [cmd commit]; // Sole commit owner; all GPU readers are in this buffer.
        return MLPyrowaveMetalSubmitSubmitted;
    }
}
- (void)stop
{
    MLPyrowaveMetalPool *p = _pool;
    os_unfair_lock_lock(&p->lock);
    Unlock unlock(&p->lock);
    p->stopped = true; // Completion still owns heaps/pipelines/queue; no CPU wait.
}
@end
