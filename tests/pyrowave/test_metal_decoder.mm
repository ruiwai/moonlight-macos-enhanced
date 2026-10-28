// SPDX-License-Identifier: GPL-3.0-only
// Synthetic coordinator smoke + analytic-zero dequant oracle, NOT encoder/full pixel correctness.
#import "MLPyrowaveMetalDecoder.h"
#include "MLPyrowavePresentationBudget.h"
#include "MLPyrowaveReceiver.hpp"
#include <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)

static uint64_t tick(void *p) { return *static_cast<uint64_t *>(p); }
static uint64_t constantClock(void *) { return 100; }
static MLPyrowaveConfiguration config(uint64_t generation = 1)
{
    return {128, 128, 256, 0, 0, generation};
}
static NSError *sentinel() { return [NSError errorWithDomain:@"previous" code:99 userInfo:nil]; }
static void invalid(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q,
                    MLPyrowaveConfiguration c, MLPyrowaveClock clock, NSString *reason)
{
    NSError *error = sentinel();
    CHECK(![MLPyrowaveMetalDecoder decoderWithKernels:k commandQueue:q configuration:c
                                              clock:clock context:nullptr error:&error]);
    CHECK([error.domain isEqualToString:MLPyrowaveMetalErrorDomain]);
    CHECK(error.code == MLPyrowaveMetalErrorPipeline);
    CHECK([error.localizedDescription containsString:reason]);
}
static void cpuFactoryTests()
{
    invalid(nil, nil, config(), nullptr, @"requires clock");
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
        invalid(nil, nil, c, constantClock, fault < 3 ? @"requires clock" : @"supported even SDR");
    }
    invalid(nil, nil, config(), constantClock, @"queue/device mismatch");
    CHECK(![MLPyrowaveMetalDecoder decoderWithKernels:nil commandQueue:nil configuration:config()
                                              clock:constantClock context:nullptr error:nil]);
    std::puts("CPU decoder factory-invalid checks passed (before hardware probe)");
}

// Minimal Common serializer fixture copied from test_coordinator.mm, not included.
// Protected latency + sparse codec, B16/N3 DATA and XOR recovery of DATA[0].
static MLPyrowaveFrame *makeFrame(uint64_t generation)
{
    const std::array<uint8_t, 24> codec = {{0x7f, 0xc0, 0x1f, 0xd0, 1, 0, 0, 0,
        1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0}};
    const std::array<uint8_t, 10> latency = {{1, 0, 0, 5, 0, 0, 0, 2, 0x12, 0x34}};
    struct Packet { std::array<uint8_t, 256> bytes = {}; size_t length = 0; };
    std::array<Packet, 4> packets;
    std::array<uint8_t, 48> bytes = {};
    std::array<uint8_t, 16> parity = {};
    std::memcpy(bytes.data(), latency.data(), latency.size());
    std::memcpy(bytes.data() + latency.size(), codec.data(), codec.size());
    for (uint16_t i = 0; i < 4; ++i) {
        LI_PYROWAVE_PACKET_HEADER h = {};
        h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
        h.frameId = 0x12345678; h.rtpTimestamp = 0x87654321;
        h.packetKind = i == 3 ? LI_PYROWAVE_PACKET_PARITY : LI_PYROWAVE_PACKET_DATA;
        h.dataBlockCount = 3; h.parityBlockCount = 1; h.blockCount = 4; h.blockIndex = i;
        h.fecBlockPayloadSize = h.payloadLength = 16;
        h.codecPayloadLength = uint32_t(codec.size()); h.metadataLength = uint16_t(latency.size());
        h.protectedPayloadLength = uint32_t(codec.size() + latency.size());
        h.metadataFlags = LI_PYROWAVE_METADATA_FLAG_PROTECTED;
        h.flags = LI_PYROWAVE_FLAG_METADATA_PRESENT;
        h.fecScheme = LI_PYROWAVE_FEC_SCHEME_XOR; h.fecDataCount = 3;
        h.fecParityCount = 1; h.fecShardIndex = uint8_t(i);
        if (i == 3) h.flags |= LI_PYROWAVE_FLAG_FEC_PARITY;
        if (i == 0) h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
        if (i == 2) h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
        const uint8_t *body = i == 3 ? parity.data() : bytes.data() + i * 16;
        auto &p = packets[i];
        CHECK(LiPyrowaveBuildPacket(&h, body, 16, p.bytes.data(), p.bytes.size(), &p.length) ==
              LI_PYROWAVE_PACKET_OK);
        if (i < 3) for (size_t j = 0; j < 16; ++j) parity[j] ^= body[j];
    }
    auto *coordinator = [[MLPyrowaveCoordinator alloc] initWithConfiguration:config(generation)
                                                                    clock:constantClock context:nullptr];
    CHECK(coordinator);
    for (unsigned i : {2u, 3u, 1u}) {
        const auto &p = packets[i];
        LI_PYROWAVE_RECEIVED_PACKET packet = {p.bytes.data(), p.length, 0x12345678, 0xabcdef01, 100, 9000};
        CHECK([coordinator submitPacket:&packet] == (i == 1 ? LI_PYROWAVE_PACKET_VALIDATED_FRAME
                                                          : LI_PYROWAVE_PACKET_IGNORED));
    }
    for (auto &p : packets) p.bytes.fill(0);
    auto *frame = [coordinator tryTakeFrame];
    [coordinator stop];
    CHECK(frame && frame.generation == generation && ![coordinator tryTakeFrame]);
    const auto *v = [frame validatedFrame]; CHECK(v && v->codec);
    const auto &f = *v->codec;
    CHECK(f.payloadSize() == 24 && f.bytes().size() == 40 && f.wordOffsets().size() == 51);
    CHECK(std::memcmp(f.bytes().data(), codec.data(), codec.size()) == 0 && f.wordOffsets()[0] == 2);
    CHECK(f.frameInfo().firstReceiveUs == 100 && f.frameInfo().deadlineUs == 100100);
    CHECK(f.frameInfo().metadata.hasHostProcessingLatency);
    CHECK(f.frameInfo().metadata.hostProcessingLatency == 0x1234);
    CHECK(f.frameInfo().frameId == 0x12345678 && f.frameInfo().rtpTimestamp == 0x87654321);
    CHECK(v->outerRtpTimestamp == 0xabcdef01 && v->presentationTimeUs == 9000);
    return frame;
}

// Captured by value: late/erroneous callbacks cannot reference stack state.
struct CompletionState {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    std::atomic<unsigned> calls{0};
    std::atomic<bool> success{false};
};
static MLPyrowaveMetalCompletion callback(std::shared_ptr<CompletionState> state)
{
    return ^(BOOL success) {
        state->success.store(success);
        CHECK(state->calls.fetch_add(1) == 0);
        dispatch_semaphore_signal(state->semaphore);
    };
}
static MLPyrowaveMetalDecoder *create(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q, uint64_t &now)
{
    NSError *error = sentinel();
    auto *decoder = [MLPyrowaveMetalDecoder decoderWithKernels:k commandQueue:q configuration:config()
                                                       clock:tick context:&now error:&error];
    if (!decoder) std::fprintf(stderr, "Decoder factory failed: %s\n", error.description.UTF8String);
    CHECK(decoder && error == nil);
    const auto c = decoder.configuration;
    CHECK(c.width == 128 && c.height == 128 && c.maxPacketSize == 256);
    CHECK(c.range == 0 && c.siting == 0 && c.generation == 1);
    return decoder;
}
// Mixed inputs: synthetic same-shader scheduling equivalence. Absent/coded-zero: analytic-zero oracle.
// Neither is independent nonzero IDWT proof or an encoder golden.
static void finishGPU(id<MTLCommandBuffer> cmd)
{
    CHECK(cmd);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(done); }];
    [cmd commit];
    CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
    CHECK(cmd.status == MTLCommandBufferStatusCompleted && !cmd.error);
}
static void dequantBatchCase(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q,
    NSUInteger width, NSUInteger height, unsigned bandCount, unsigned fixtureMode)
{
    // Fixture modes: 0 = original mixed, 1 = absent, 2 = coded-zero with poisoned unused bytes.
    struct Push { int32_t resolution[2], outputLayer, blockOffset, blockStride, blockLayerStride; };
    static_assert(sizeof(Push) == 24, "Dequant ABI including former padding");
    const unsigned firstBand = 4 - bandCount, base = 3;
    const unsigned blocksX = (width + 31) / 32, blocksY = (height + 31) / 32;
    const unsigned layerStride = blocksX * blocksY, records = bandCount * layerStride;
    id<MTLBuffer> input = [k.device newBufferWithLength:8 + records * 16 + 16
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> offsets = [k.device newBufferWithLength:(base + records) * 4
                                                 options:MTLResourceStorageModeShared];
    CHECK(input && input.contents && offsets && offsets.contents);
    std::memset(input.contents, 0, input.length); // Sequence placeholder + trailing shader read guards.
    std::memset(offsets.contents, 0xff, offsets.length); // Absent blocks remain UINT32_MAX.
    auto *table = static_cast<uint32_t *>(offsets.contents);
    auto *bytes = static_cast<uint8_t *>(input.contents);
    for (unsigned b = 0; b < bandCount; ++b) {
        for (unsigned block = 0; block < layerStride; ++block) {
            if (fixtureMode == 1 || (fixtureMode == 0 &&
                ((b + block) % 3 == 1 && block != layerStride - 1))) continue;
            // The 16-byte record after makeFrame's 8-byte sequence header; not a GPU encoder fixture.
            std::array<uint8_t, 16> record = {{1, 0, 4, 0x50, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0}};
            record[4] = uint8_t(8 * (b + 1) + block); // Distinct band/block quantization.
            record[8] = 1; record[9] = 0; // Ballot bit 0: first group has one bitplane.
            record[10] = 0; record[11] = 3; // q_bits=0, scale=0; two nonzero coefficients.
            record[12] = 1; // First coefficient negative, second positive; independent sign data.
            if (fixtureMode == 2) {
                record.fill(0xa5); // Unused control/planes/signs must not affect the zero result.
                record[0] = record[1] = 0; // Valid record offset, zero low 16-bit ballot.
                record[4] = uint8_t(8 * (b + 1) + block); // Existing legal quantization.
            }
            const unsigned byteOffset = 8 + 16 * (records - 1 - (b * layerStride + block));
            std::memcpy(bytes + byteOffset, record.data(), record.size());
            table[base + b * layerStride + block] = byteOffset / 4; // Shader offsets are words.
        }
    }
    auto *desc = [MTLTextureDescriptor new];
    desc.textureType = MTLTextureType2DArray; desc.pixelFormat = MTLPixelFormatR32Float;
    desc.width = width; desc.height = height; desc.arrayLength = 4;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    id<MTLTexture> outputs[] = {[k.device newTextureWithDescriptor:desc],
                              [k.device newTextureWithDescriptor:desc],
                              [k.device newTextureWithDescriptor:desc]};
    CHECK(outputs[0] && outputs[1] && outputs[2]);
    const NSUInteger rowBytes = ((width * sizeof(float) + 255) / 256) * 256;
    const NSUInteger imageBytes = rowBytes * height, textureBytes = 4 * imageBytes;
    id<MTLBuffer> readback = [k.device newBufferWithLength:3 * textureBytes
                                                  options:MTLResourceStorageModeShared];
    CHECK(readback && readback.contents);
    std::memset(readback.contents, 0x7f, readback.length); // Sentinel detects unwritten valid pixels.
    id<MTLCommandBuffer> cmd = [q commandBuffer]; CHECK(cmd);
    id<MTLBlitCommandEncoder> init = [cmd blitCommandEncoder]; CHECK(init);
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned layer = 0; layer < 4; ++layer)
            [init copyFromBuffer:readback sourceOffset:0 sourceBytesPerRow:rowBytes
             sourceBytesPerImage:imageBytes sourceSize:MTLSizeMake(width, height, 1)
                       toTexture:outputs[mode] destinationSlice:layer destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    [init endEncoding];
    // 0: serial per-band; 1: serial batched; 2: concurrent per-band via disjoint aliased views.
    for (unsigned mode = 0; mode < 3; ++mode) {
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoderWithDispatchType:
            mode == 2 ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial];
        CHECK(enc);
        [enc setComputePipelineState:k.dequant];
        [enc setBuffer:input offset:0 atIndex:0]; [enc setBuffer:offsets offset:0 atIndex:2];
        [enc setTexture:outputs[mode] atIndex:0];
        const unsigned dispatches = mode == 1 ? 1 : bandCount;
        for (unsigned b = 0; b < dispatches; ++b) {
            Push push = {{int32_t(width), int32_t(height)}, int32_t(firstBand + b),
                int32_t(base + b * layerStride), int32_t(blocksX), int32_t(layerStride)};
            if (mode == 2) {
                id<MTLTexture> view = [outputs[2] newTextureViewWithPixelFormat:MTLPixelFormatR32Float
                    textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1)
                    slices:NSMakeRange(firstBand + b, 1)];
                CHECK(view); // Normal command-buffer retention keeps the bound view alive.
                [enc setTexture:view atIndex:0];
                push.outputLayer = 0; // One dispatched slice maps to the matching parent slice.
            }
            [enc setBytes:&push length:sizeof(push) atIndex:1];
            [enc dispatchThreadgroups:MTLSizeMake(blocksX, blocksY, mode == 1 ? bandCount : 1)
                threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        }
        [enc endEncoding];
    }
    finishGPU(cmd);
    // Read back only after compute completion, then wait for the blits before CPU inspection.
    cmd = [q commandBuffer]; CHECK(cmd);
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder]; CHECK(blit);
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned layer = 0; layer < 4; ++layer)
            [blit copyFromTexture:outputs[mode] sourceSlice:layer sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                         toBuffer:readback destinationOffset:mode * textureBytes + layer * imageBytes
           destinationBytesPerRow:rowBytes destinationBytesPerImage:imageBytes];
    [blit endEncoding];
    finishGPU(cmd);
    const auto *result = static_cast<const uint8_t *>(readback.contents);
    bool positive = false, negative = false;
    for (unsigned layer = firstBand; layer < 4; ++layer) {
        for (NSUInteger y = 0; y < height; ++y) {
            const auto *row = result + layer * imageBytes + y * rowBytes;
            CHECK(std::memcmp(row, row + textureBytes, width * sizeof(float)) == 0);
            CHECK(std::memcmp(row, row + 2 * textureBytes, width * sizeof(float)) == 0);
            for (NSUInteger x = 0; x < width; ++x) {
                uint32_t bits; float value;
                std::memcpy(&bits, row + x * 4, 4); std::memcpy(&value, &bits, 4);
                CHECK(bits != 0x7f7f7f7f);
                if (fixtureMode != 0) CHECK(bits == 0u); // Exact +0 in all three outputs via memcmp above.
                const unsigned block = unsigned(y / 32) * blocksX + unsigned(x / 32);
                if (table[base + (layer - firstBand) * layerStride + block] == UINT32_MAX)
                    CHECK(value == 0.0f);
                positive |= value > 0; negative |= value < 0;
            }
        }
    }
    CHECK(fixtureMode != 0 ? (!positive && !negative) : (positive && negative));
}
static void dequantBatchTests(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q)
{
    for (unsigned bands : {3u, 4u})
        for (unsigned width : {4u, 65u})
            for (unsigned fixtureMode : {0u, 1u, 2u}) {
                @autoreleasepool { dequantBatchCase(k, q, width, width == 4 ? 4 : 33, bands, fixtureMode); }
            }
    std::puts("Dequant 3-strategy scheduling equivalence + analytic-zero oracle passed; NOT independent nonzero IDWT/encoder proof");
}
static void hardwareTests(MLPyrowaveMetalKernels *k, id<MTLCommandQueue> q)
{
    invalid(k, nil, config(), constantClock, @"queue/device mismatch");
    invalid(nil, q, config(), constantClock, @"queue/device mismatch");
    auto large = config(); large.width = large.height = 16384;
    invalid(k, q, large, constantClock, @"512 MiB"); // Codec-valid, decoder budget rejects.
    for (uint32_t width : {1920u, 3840u}) {
        @autoreleasepool { // Release each fixed allocation before the next size.
            auto requested = config();
            requested.width = width; requested.height = width == 1920 ? 1080 : 2160;
            NSError *error = sentinel();
            auto *decoder = [MLPyrowaveMetalDecoder decoderWithKernels:k commandQueue:q configuration:requested
                                                               clock:constantClock context:nullptr error:&error];
            if (!decoder) std::fprintf(stderr, "%ux%u allocation failed: %s\n",
                requested.width, requested.height, error.description.UTF8String);
            CHECK(decoder && error == nil);
            const auto actual = decoder.configuration;
            CHECK(actual.width == requested.width && actual.height == requested.height);
            CHECK(actual.maxPacketSize == requested.maxPacketSize && actual.generation == requested.generation);
            CHECK(actual.range == requested.range && actual.siting == requested.siting);
            CHECK(decoder.sessionAllocationBytes > 0 &&
                  decoder.sessionAllocationBytes <= MLPyrowaveSessionAllocationCap);
            [decoder stop];
        }
    }
    uint64_t now = 100; // Borrowed context outlives both decoder autorelease pools.
    auto *frame = makeFrame(1);
    auto rejected = std::make_shared<CompletionState>();
    MLPyrowaveMetalCompletion noCompletion = callback(rejected);
    __block unsigned presentations = 0; // Presentation is synchronous only.
    id<MTLBuffer> readback = [k.device newBufferWithLength:256 * (128 + 64 + 64)
                                                options:MTLResourceStorageModeShared];
    CHECK(readback && readback.contents);
    MLPyrowaveMetalPresentation present = ^BOOL(id<MTLCommandBuffer> cmd,
        id<MTLTexture> y, id<MTLTexture> cb, id<MTLTexture> cr) {
        ++presentations;
        CHECK(cmd.commandQueue == q);
        id<MTLTexture> planes[] = {y, cb, cr}; // Borrow only; never escape this block.
        id<MTLBlitCommandEncoder> enc = [cmd blitCommandEncoder]; CHECK(enc);
        NSUInteger offset = 0;
        for (unsigned i = 0; i < 3; ++i) {
            id<MTLTexture> t = planes[i];
            const NSUInteger side = i ? 64 : 128;
            CHECK(t.device == k.device && t.textureType == MTLTextureType2D);
            CHECK(t.pixelFormat == MTLPixelFormatR8Unorm && t.width == side && t.height == side);
            CHECK(t.depth == 1 && t.arrayLength == 1 && t.mipmapLevelCount == 1);
            CHECK(t.sampleCount == 1 && t.storageMode == MTLStorageModePrivate);
            [enc copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(side, side, 1) toBuffer:readback destinationOffset:offset
          destinationBytesPerRow:256 destinationBytesPerImage:256 * side];
            offset += 256 * side;
        }
        CHECK(offset == readback.length);
        [enc endEncoding];
        return YES; // No commit/enqueue, retained planes/command buffer, or control reentry.
    };
    @autoreleasepool {
        auto *decoder = create(k, q, now);
        CHECK([decoder submitFrame:nil presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        CHECK([decoder submitFrame:frame presentation:nil completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        @autoreleasepool {
            auto *wrong = makeFrame(2);
            CHECK([decoder submitFrame:wrong presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        }
        CHECK(presentations == 0);
        MLPyrowaveMetalPresentation decline = ^BOOL(id<MTLCommandBuffer> cmd,
            id<MTLTexture> y, id<MTLTexture> cb, id<MTLTexture> cr) {
            ++presentations; CHECK(cmd && y && cb && cr); return NO;
        };
        CHECK([decoder submitFrame:frame presentation:decline completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        CHECK(presentations == 1);
        CHECK(decoder.lastError == nil);
        for (unsigned i = 0; i < 2; ++i) { // Re-submit only AFTER actual completion, no assumed Busy timing.
            auto state = std::make_shared<CompletionState>();
            CHECK([decoder submitFrame:frame presentation:present completion:callback(state)] == MLPyrowaveMetalSubmitSubmitted);
            CHECK(dispatch_semaphore_wait(state->semaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
            CHECK(state->calls.load() == 1 && state->success.load());
            CHECK(decoder.lastError == nil);
        }
        CHECK(presentations == 3);
        now = 100100; // Exact expiry drops without presenting or completing.
        CHECK([decoder submitFrame:frame presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        ++now;
        CHECK([decoder submitFrame:frame presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        [decoder stop]; [decoder stop];
        CHECK([decoder submitFrame:frame presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitStopped);
        CHECK(presentations == 3);
    } // Drain the decoder's autorelease pool before replacement.
    now = 100;
    @autoreleasepool {
        auto *decoder = create(k, q, now);
        MLPyrowaveMetalPresentation decline = ^BOOL(id<MTLCommandBuffer> cmd,
            id<MTLTexture> y, id<MTLTexture> cb, id<MTLTexture> cr) {
            CHECK(cmd && y && cb && cr); return NO;
        };
        CHECK([decoder submitFrame:frame presentation:decline completion:noCompletion] == MLPyrowaveMetalSubmitDropped);
        now = 99;
        CHECK([decoder submitFrame:frame presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitStopped);
        now = 101;
        CHECK([decoder submitFrame:frame presentation:present completion:noCompletion] == MLPyrowaveMetalSubmitStopped);
        [decoder stop]; [decoder stop];
    }
    CHECK(presentations == 3);
    CHECK(dispatch_semaphore_wait(rejected->semaphore, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0);
    CHECK(rejected->calls.load() == 0); // Bounded observation, not proof about all future time.
}
int main()
{
    @autoreleasepool {
        cpuFactoryTests();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice(); // Standalone test only.
        bool supported = false;
        if (@available(macOS 13.0, *))
            supported = device && [device supportsFamily:MTLGPUFamilyApple7] &&
                device.maxThreadsPerThreadgroup.width >= 128;
        if (!supported) {
            std::puts("SKIP (77): CPU factory checks passed; macOS 13/Apple7 Metal device unavailable");
            return 77;
        }
        NSError *error = sentinel();
        auto *kernels = [MLPyrowaveMetalKernels kernelsWithDevice:device error:&error];
        if (!kernels) std::fprintf(stderr, "Kernel factory failed: %s\n", error.description.UTF8String);
        CHECK(kernels && error == nil); // Supported-device failures FAIL, never skip.
        id<MTLCommandQueue> queue = [device newCommandQueue]; CHECK(queue);
        dequantBatchTests(kernels, queue);
        hardwareTests(kernels, queue);
        std::puts("Metal decoder synthetic submission/completion smoke passed; no pixel correctness claim");
        return 0;
    }
}
