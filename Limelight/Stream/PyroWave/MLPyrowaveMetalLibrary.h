// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "MLPyrowaveMetalKernels.h"
#include <CommonCrypto/CommonDigest.h>

// Internal bundle-only lookup. A stale/unloadable artifact never blocks source fallback.
static inline id<MTLLibrary> MLPyrowaveLoadLibrary(id<MTLDevice> device, NSBundle *bundle,
    NSString *name, const char *source, MTLCompileOptions *options, NSError **error)
{
    if (error) *error = nil;
    NSString *text = source ? [NSString stringWithUTF8String:source] : nil;
    if (!text) {
        if (error) *error = [NSError errorWithDomain:MLPyrowaveMetalErrorDomain
            code:MLPyrowaveMetalErrorPipeline userInfo:@{NSLocalizedDescriptionKey:
                @"PyroWave shader source is unavailable or invalid UTF-8"}];
        return nil;
    }
    NSData *prefix = [@"pyrowave-metal3.0-fp32-v1\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *sourceBytes = [text dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableData *bytes = prefix && sourceBytes ? [NSMutableData dataWithData:prefix] : nil;
    NSMutableString *expected = bytes ? [NSMutableString stringWithCapacity:64] : nil;
    if (expected) {
        [bytes appendData:sourceBytes];
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        if (CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest)) {
            for (unsigned char byte : digest) [expected appendFormat:@"%02x", (unsigned int)byte];
        } else expected = nil;
    }
    if (expected) { // Any fingerprint allocation failure skips bundle lookup entirely.
        NSURL *libraryURL = [bundle URLForResource:name withExtension:@"metallib" subdirectory:@"PyroWave"];
        NSURL *signatureURL = [bundle URLForResource:name withExtension:@"sha256" subdirectory:@"PyroWave"];
        NSString *actual = signatureURL ? [NSString stringWithContentsOfURL:signatureURL
            encoding:NSUTF8StringEncoding error:nil] : nil;
        if (libraryURL && [actual isEqualToString:expected]) {
            id<MTLLibrary> library = [device newLibraryWithURL:libraryURL error:error];
            if (library) {
                if (error) *error = nil;
                library.label = @"PyroWave bundled";
                return library;
            }
        }
    }
    if (error) *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:error];
    if (library) {
        if (error) *error = nil;
        library.label = @"PyroWave runtime";
    }
    return library;
}
