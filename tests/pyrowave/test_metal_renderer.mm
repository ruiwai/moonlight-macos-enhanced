// SPDX-License-Identifier: GPL-3.0-only
// Actual-source synthetic zero-coefficient smoke, NOT full encoder correctness.
#import "MLPyrowaveMetalRenderer.h"
#include "MLPyrowavePresentationBudget.h"
#include <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)

static uint64_t clockNow(void *) { return 100; }
static MLPyrowaveConfiguration config() { return {128, 128, 256, 0, 0, 1}; }
static NSError *sentinel() { return [NSError errorWithDomain:@"previous" code:99 userInfo:nil]; }
static void invalid(MLPyrowaveConfiguration c, MLPyrowaveClock clock, NSString *reason)
{
    NSError *error = sentinel();
    CHECK(![MLPyrowaveMetalRenderer rendererWithCommandQueue:nil configuration:c
        clock:clock context:nullptr error:&error]);
    CHECK([error.domain isEqualToString:MLPyrowaveMetalErrorDomain]);
    CHECK(error.code == MLPyrowaveMetalErrorPipeline);
    CHECK([error.localizedDescription containsString:reason]);
}
static void cpuFactoryTests()
{
    const uint64_t cap = MLPyrowaveSessionAllocationCap;
    CHECK(!MLPyrowavePresentationFitsBudget(0, 1, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(1, 0, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(1, 1, 0));
    CHECK(MLPyrowavePresentationFitsBudget(cap - 2, 1, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(cap - 1, 1, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(cap + 1, 1, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(UINT64_MAX, 1, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(1, UINT64_MAX, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(1, 2, UINT64_MAX));
    invalid(config(), nullptr, @"requires clock");
    for (unsigned fault = 0; fault < 11; ++fault) {
        auto c = config();
        if (fault == 0) c.generation = 0;
        if (fault == 1) c.maxPacketSize = 128;
        if (fault == 2) c.maxPacketSize = 65537;
        if (fault == 3) c.width = 0;
        if (fault == 4) c.width = 127;
        if (fault == 5) c.height = 0;
        if (fault == 6) c.height = 127;
        if (fault == 7) c.width = 16386;
        if (fault == 8) c.height = 16386;
        if (fault == 9) c.range = 2;
        if (fault == 10) c.siting = 2;
        invalid(c, clockNow, fault < 3 ? @"requires clock" : @"supported even SDR");
    }
    invalid(config(), clockNow, @"actual queue/device");
    CHECK(![MLPyrowaveMetalRenderer rendererWithCommandQueue:nil configuration:config()
        clock:clockNow context:nullptr error:nil]);
    std::puts("CPU renderer factory-invalid checks passed (before hardware probe)");
}
// sequence(s, 0) from test_codec: valid 8-byte header-only sparse zero count.
// Common serializer, B16/N1/parity1 XOR, padded DATA, no metadata/FRAME_HEADER.
static int submitZero(MLPyrowaveMetalRenderer *renderer, MLPyrowaveConfiguration c,
                      uint32_t frameId, bool recover = false)
{
    std::array<uint8_t, 16> body = {};
    const uint32_t words[] = {(c.width - 1) | ((c.height - 1) << 14) | (3u << 28) | (1u << 31),
        (uint32_t(c.range) << 30) | (uint32_t(c.siting) << 31)};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 4; ++j) body[i * 4 + j] = uint8_t(words[i] >> (j * 8));
    LI_PYROWAVE_PACKET_HEADER h = {};
    h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
    h.frameId = frameId; h.rtpTimestamp = 0x12345678;
    h.packetKind = recover ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
    h.flags = recover ? LI_PYROWAVE_FLAG_FEC_PARITY :
        LI_PYROWAVE_FLAG_START_OF_FRAME | LI_PYROWAVE_FLAG_END_OF_FRAME;
    h.dataBlockCount = 1; h.parityBlockCount = 1; h.blockCount = 2;
    h.blockIndex = recover ? 1 : 0;
    h.fecBlockPayloadSize = h.payloadLength = 16;
    h.codecPayloadLength = h.protectedPayloadLength = 8;
    h.fecScheme = LI_PYROWAVE_FEC_SCHEME_XOR; h.fecDataCount = h.fecParityCount = 1;
    h.fecShardIndex = uint8_t(h.blockIndex); // N1 XOR parity equals padded DATA.
    std::array<uint8_t, 256> bytes = {};
    size_t length = 0;
    CHECK(LiPyrowaveBuildPacket(&h, body.data(), body.size(), bytes.data(), bytes.size(), &length) ==
          LI_PYROWAVE_PACKET_OK);
    LI_PYROWAVE_RECEIVED_PACKET packet = {bytes.data(), length, frameId, 0xabcdef01, 100, 9000};
    const int ack = [renderer submitPacket:&packet];
    bytes.fill(0); body.fill(0); // No borrowed packet bytes survive this call.
    return ack;
}
struct State {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    std::atomic<unsigned> calls{0};
    std::atomic<bool> success{false};
};
static MLPyrowaveMetalCompletion callback(std::shared_ptr<State> state)
{
    return ^(BOOL success) {
        state->success.store(success);
        CHECK(state->calls.fetch_add(1) == 0);
        dispatch_semaphore_signal(state->semaphore);
    };
}
static void await(std::shared_ptr<State> state)
{
    CHECK(dispatch_semaphore_wait(state->semaphore,
          dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
    CHECK(state->calls.load() == 1 && state->success.load());
}
static MTLTextureDescriptor *targetDescriptor(NSUInteger width, NSUInteger height)
{
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:width height:height mipmapped:NO];
    CHECK(d); d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget;
    return d;
}
static void checkPixels(id<MTLCommandQueue> queue, id<MTLTexture> target,
                        NSUInteger width, NSUInteger height, MLPyrowaveConfiguration c)
{
    CHECK(target.width == width && target.height == height);
    const NSUInteger row = (width * 4 + 255) & ~NSUInteger(255);
    id<MTLBuffer> buffer = [queue.device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
    CHECK(buffer && buffer.contents);
    id<MTLCommandBuffer> cmd = [queue commandBuffer]; CHECK(cmd && cmd.commandQueue == queue);
    id<MTLBlitCommandEncoder> enc = [cmd blitCommandEncoder]; CHECK(enc);
    [enc copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [enc endEncoding];
    auto state = std::make_shared<State>();
    auto done = callback(state);
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        done(completed.status == MTLCommandBufferStatusCompleted);
    }];
    [cmd commit]; await(state); // Test-only readback AFTER renderer completion, same queue.
    const auto *pixels = static_cast<const uint8_t *>(buffer.contents);
    const int expected = c.range ? int(std::lround((128.0 - 16.0) * 255.0 / 219.0)) : 128;
    const double scale = std::min(double(width) / c.width, double(height) / c.height);
    const double w = c.width * scale, h = c.height * scale;
    const NSUInteger left = std::ceil((width - w) * .5), top = std::ceil((height - h) * .5);
    const NSUInteger right = std::floor((width + w) * .5), bottom = std::floor((height + h) * .5);
    CHECK(left < right && right <= width && top < bottom && bottom <= height);
    auto pixel = [&](NSUInteger x, NSUInteger y, int value) {
        CHECK(x < width && y < height);
        const auto *p = pixels + y * row + x * 4;
        for (unsigned channel = 0; channel < 3; ++channel)
            CHECK(std::abs(int(p[channel]) - value) <= 2);
        CHECK(p[3] == 255);
    };
    const NSUInteger midX = (left + right) / 2, midY = (top + bottom) / 2;
    for (NSUInteger x : {left, midX, right - 1})
        for (NSUInteger y : {top, midY, bottom - 1}) pixel(x, y, expected);
    if (left) { pixel(0, midY, 0); pixel(left - 1, midY, 0); }
    if (right < width) { pixel(right, midY, 0); pixel(width - 1, midY, 0); }
    if (top) { pixel(midX, 0, 0); pixel(midX, top - 1, 0); }
    if (bottom < height) { pixel(midX, bottom, 0); pixel(midX, height - 1, 0); }
}
static void hardwareTests(id<MTLCommandQueue> queue, MLPyrowaveConfiguration c)
{
    NSError *error = sentinel();
    auto *renderer = [MLPyrowaveMetalRenderer rendererWithCommandQueue:queue configuration:c
        clock:clockNow context:nullptr error:&error];
    if (!renderer) std::fprintf(stderr, "Renderer factory failed: %s\n", error.description.UTF8String);
    CHECK(renderer && error == nil);
    CHECK([renderer canPresentWidth:256 height:128]);
    CHECK(![renderer canPresentWidth:0 height:128]);
    CHECK(![renderer canPresentWidth:128 height:0]);
    CHECK(![renderer canPresentWidth:16385 height:128]);
    CHECK(![renderer canPresentWidth:128 height:16385]);
    CHECK(![renderer canPresentWidth:NSUIntegerMax height:128]);
    CHECK(![renderer canPresentWidth:16384 height:16384]); // Two targets alone exceed cap.
    const auto actual = renderer.configuration;
    CHECK(actual.width == c.width && actual.height == c.height && actual.range == c.range &&
          actual.siting == c.siting && actual.generation == c.generation && actual.maxPacketSize == c.maxPacketSize);
    id<MTLDevice> device = queue.device;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor(256, 128)]; CHECK(target);
    const uint64_t bytes = target.allocatedSize, cap = MLPyrowaveSessionAllocationCap;
    CHECK(bytes && bytes < cap / 2);
    CHECK(MLPyrowavePresentationFitsBudget(cap - 2 * bytes, bytes, 2));
    CHECK(!MLPyrowavePresentationFitsBudget(cap - 2 * bytes + 1, bytes, 2));
    id<MTLTexture> emptyFit = [device newTextureWithDescriptor:targetDescriptor(1, 2)]; CHECK(emptyFit);
    auto *memorylessDescriptor = targetDescriptor(256, 128);
    memorylessDescriptor.storageMode = MTLStorageModeMemoryless;
    id<MTLTexture> memoryless = [device newTextureWithDescriptor:memorylessDescriptor];
    if (!memoryless) std::puts("Memoryless target subcase unavailable: actual allocation unsupported");
    else CHECK(memoryless.storageMode == MTLStorageModeMemoryless);
    auto rejected = std::make_shared<State>();
    auto noCompletion = callback(rejected);
    CHECK([renderer submitPacket:nullptr] == LI_PYROWAVE_PACKET_IGNORED);
    CHECK([renderer renderPendingToTexture:target completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
    const std::array<NSUInteger, 2> sizes[] = {{256, 128}, {128, 256}, {257, 130}, {131, 257}};
    uint32_t frameId = 0;
    for (const auto &size : sizes) {
        ++frameId;
        target = [device newTextureWithDescriptor:targetDescriptor(size[0], size[1])]; CHECK(target);
        CHECK(submitZero(renderer, c, frameId, frameId == 2) == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
        CHECK([renderer submitPacket:nullptr] == LI_PYROWAVE_PACKET_IGNORED);
        CHECK([renderer renderPendingToDrawable:nil completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        CHECK([renderer renderPendingToTexture:nil completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        CHECK([renderer renderPendingToTexture:emptyFit completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        if (memoryless)
            CHECK([renderer renderPendingToTexture:memoryless completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        for (unsigned fault = 0; fault < 6; ++fault) {
            auto *d = targetDescriptor(size[0], size[1]);
            if (fault == 0) d.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
            if (fault == 1) d.pixelFormat = MTLPixelFormatRGBA8Unorm;
            if (fault == 2) d.usage = MTLTextureUsageShaderRead;
            if (fault == 3) d.mipmapLevelCount = 2;
            if (fault == 4) { d.textureType = MTLTextureType2DArray; d.arrayLength = 2; }
            if (fault == 5) {
                CHECK([device supportsTextureSampleCount:4]);
                d.textureType = MTLTextureType2DMultisample; d.sampleCount = 4;
            }
            id<MTLTexture> invalidTarget = [device newTextureWithDescriptor:d]; CHECK(invalidTarget);
            CHECK([renderer renderPendingToTexture:invalidTarget completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        }
        bool checkedForeignDevice = false;
        for (id<MTLDevice> other in MTLCopyAllDevices()) {
            if (other == device) continue;
            id<MTLTexture> foreign = [other newTextureWithDescriptor:targetDescriptor(size[0], size[1])]; CHECK(foreign);
            CHECK([renderer renderPendingToTexture:foreign completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
            checkedForeignDevice = true;
        }
        if (!checkedForeignDevice) std::puts("Foreign-device target check unavailable: only one Metal device");
        auto state = std::make_shared<State>();
        CHECK([renderer renderPendingToTexture:target completion:callback(state)] == MLPyrowaveMetalSubmitSubmitted);
        await(state); checkPixels(queue, target, size[0], size[1], c);
        CHECK([renderer renderPendingToTexture:target completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
    }
    CHECK(submitZero(renderer, c, ++frameId) == LI_PYROWAVE_PACKET_VALIDATED_FRAME);
    [renderer stop]; [renderer stop]; // Clears coordinator pending; stops both components permanently.
    CHECK(submitZero(renderer, c, ++frameId) == LI_PYROWAVE_PACKET_IGNORED);
    CHECK([renderer renderPendingToTexture:target completion:noCompletion] == MLPyrowaveMetalSubmitStopped);
    CHECK([renderer renderPendingToDrawable:nil completion:noCompletion] == MLPyrowaveMetalSubmitStopped);
    CHECK(dispatch_semaphore_wait(rejected->semaphore,
          dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC)) != 0);
    CHECK(rejected->calls.load() == 0); // Bounded observation, not a claim about all future time.
}
int main()
{
    @autoreleasepool {
        cpuFactoryTests();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice(); // Standalone test ONLY.
        bool supported = false;
        if (@available(macOS 13.0, *))
            supported = device && [device supportsFamily:MTLGPUFamilyApple7] &&
                device.maxThreadsPerThreadgroup.width >= 128;
        if (!supported) {
            std::puts("SKIP (77): CPU factory checks passed; macOS 13/Apple7 Metal device unavailable");
            return 77;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue]; CHECK(queue && queue.device == device);
        for (unsigned mode = 0; mode < 4; ++mode) {
            @autoreleasepool {
                auto c = config(); c.range = uint8_t(mode & 1); c.siting = uint8_t(mode >> 1);
                hardwareTests(queue, c);
            } // Drain old session before replacement, no overlapping decoder budgets.
        }
        std::puts("Metal renderer offscreen zero-frame/range/letterbox smoke passed; NOT full pixel correctness or UI presentation");
    }
    return 0;
}
