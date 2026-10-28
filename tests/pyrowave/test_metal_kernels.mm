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

static void CheckLoaderFallbacks(id<MTLDevice> device, NSBundle *packaged, MTLCompileOptions *options)
{
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *name = @"pyrowave-dequant";
    NSURL *signature = [packaged URLForResource:name withExtension:@"sha256" subdirectory:@"PyroWave"];
    CHECK(signature != nil);
    NSData *correct = [NSData dataWithContentsOfURL:signature];
    CHECK(correct.length == 64);
    for (unsigned mode = 0; mode < 3; ++mode) {
        // Fresh paths AND identifiers avoid NSBundle resource-cache reuse.
        NSString *identifier = [@"com.moonlight.pyrowave.fallback." stringByAppendingString:NSUUID.UUID.UUIDString];
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [identifier stringByAppendingPathExtension:@"bundle"]];
        NSString *contents = [path stringByAppendingPathComponent:@"Contents"];
        NSString *resources = [contents stringByAppendingPathComponent:@"Resources/PyroWave"];
        CHECK(![files fileExistsAtPath:path]);
        CHECK([files createDirectoryAtPath:resources withIntermediateDirectories:YES attributes:nil error:nil]);
        NSDictionary *info = @{@"CFBundleIdentifier": identifier, @"CFBundlePackageType": @"BNDL"};
        CHECK([info writeToFile:[contents stringByAppendingPathComponent:@"Info.plist"] atomically:YES]);
        if (mode != 0) {
            CHECK([[@"invalid metallib" dataUsingEncoding:NSUTF8StringEncoding]
                writeToFile:[resources stringByAppendingPathComponent:@"pyrowave-dequant.metallib"] atomically:YES]);
            NSData *fingerprint = mode == 1 ? [@"stale" dataUsingEncoding:NSUTF8StringEncoding] : correct;
            CHECK([fingerprint writeToFile:[resources stringByAppendingPathComponent:@"pyrowave-dequant.sha256"]
                atomically:YES]);
        }
        NSBundle *fixture = [NSBundle bundleWithPath:path];
        CHECK(fixture != nil && [fixture.bundleIdentifier isEqualToString:identifier]);
        if (mode != 0) {
            CHECK([fixture URLForResource:name withExtension:@"metallib" subdirectory:@"PyroWave"] != nil);
            CHECK([fixture URLForResource:name withExtension:@"sha256" subdirectory:@"PyroWave"] != nil);
        }
        NSError *error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
        id<MTLLibrary> library = MLPyrowaveLoadLibrary(device, fixture, name,
            wavelet_dequant_msl_source, options, &error);
        if (!library) std::fprintf(stderr, "Fallback %u failed: %s\n", mode, error.description.UTF8String);
        CHECK(library != nil && error == nil);
        CHECK([library.label isEqualToString:@"PyroWave runtime"]);
        CHECK([files removeItemAtPath:path error:nil]);
    }
    NSError *error = [NSError errorWithDomain:@"previous" code:99 userInfo:nil];
    CHECK(MLPyrowaveLoadLibrary(device, packaged, name, nullptr, options, &error) == nil);
    CHECK([error.domain isEqualToString:MLPyrowaveMetalErrorDomain]);
    CHECK(error.code == MLPyrowaveMetalErrorPipeline);
}

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
            if (bundled) CheckLoaderFallbacks(device, bundle, options);
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
