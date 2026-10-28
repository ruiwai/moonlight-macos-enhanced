// SPDX-License-Identifier: GPL-3.0-only
// Actual pipeline creation only: no decode dispatch or pixel validation.
#import "MLPyrowaveMetalKernels.h"
#include "MLPyrowaveMetalLibrary.h"
#include "MLPyrowaveMetalShaders.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); \
} } while (0)

int main(int argc, char **argv)
{
    const bool bundled = argc == 2 && std::strcmp(argv[1], "--bundled") == 0;
    CHECK(argc == 1 || bundled);
    @autoreleasepool {
        NSError *error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:nil error:&error] == nil);
        CHECK([error.domain isEqualToString:MLPyrowaveMetalErrorDomain]);
        CHECK(error.code == MLPyrowaveMetalErrorUnsupportedDevice);
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:nil error:nil] == nil);
        // Default device creation is confined to this standalone test.
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        bool supported = false;
        if (@available(macOS 13.0, *)) {
            supported = device && [device supportsFamily:MTLGPUFamilyApple7] &&
                device.maxThreadsPerThreadgroup.width >= 128;
        }
        if (!supported) {
            std::puts("SKIP: supported Metal device unavailable; no GPU pipeline checks run");
            return 77;
        }
        if (@available(macOS 13.0, *)) {
            MTLCompileOptions *options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion3_0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            options.fastMathEnabled = NO;
#pragma clang diagnostic pop
            NSBundle *bundle = bundled ? [NSBundle bundleForClass:MLPyrowaveMetalKernels.class] : nil;
            NSString *names[] = {@"pyrowave-idwt", @"pyrowave-dequant"};
            const char *sources[] = {idwt_msl_source, wavelet_dequant_msl_source};
            for (unsigned i = 0; i < 2; ++i) {
                error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
                id<MTLLibrary> library = MLPyrowaveLoadLibrary(device, bundle, names[i], sources[i], options, &error);
                if (!library) std::fprintf(stderr, "Library failed: %s\n", error.description.UTF8String);
                CHECK(library != nil && error == nil);
                CHECK([library.label isEqualToString:bundled ? @"PyroWave bundled" : @"PyroWave runtime"]);
            }
        }
        error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
        MLPyrowaveMetalKernels *kernels = [MLPyrowaveMetalKernels kernelsWithDevice:device error:&error];
        if (!kernels) std::fprintf(stderr, "Pipeline creation failed: %s\n", error.description.UTF8String);
        CHECK(kernels != nil); // Supported-device failures must abort, never skip.
        CHECK(error == nil); // Factory clears a caller's previous error on entry.
        CHECK(kernels.device == device);
        CHECK(kernels.dequant != nil && kernels.dequant.device == device);
        CHECK(kernels.dequant.threadExecutionWidth == 32);
        CHECK(kernels.dequant.maxTotalThreadsPerThreadgroup >= 128);
        CHECK(kernels.idwt != nil && kernels.idwt.device == device);
        CHECK(kernels.idwt.maxTotalThreadsPerThreadgroup >= 64);
        CHECK(kernels.idwtFinal != nil && kernels.idwtFinal.device == device);
        CHECK(kernels.idwtFinal.maxTotalThreadsPerThreadgroup >= 64);
        CHECK(kernels.dequant.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength);
        CHECK(kernels.idwt.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength);
        CHECK(kernels.idwtFinal.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength);
        CHECK(kernels.sampler != nil && kernels.sampler.device == device);
        error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:device error:&error] == kernels);
        CHECK(error == nil); // Cache hits clear stale errors too.
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:nil error:&error] == nil);
        CHECK([error.domain isEqualToString:MLPyrowaveMetalErrorDomain]);
        CHECK(error.code == MLPyrowaveMetalErrorUnsupportedDevice);
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:nil error:nil] == nil);
        CHECK([MLPyrowaveMetalKernels kernelsWithDevice:device error:&error] == kernels);
        CHECK(error == nil); // Nil failures did not evict the last good bundle.
        std::puts("Metal pipeline creation checks passed (no decode dispatch/pixel validation)");
        return 0;
    }
}
