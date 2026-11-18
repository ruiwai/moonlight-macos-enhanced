#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>

#import "Logger.h"
#import "MLH264Recorder.h"

// Don't link the application's logger (or any of its filesystem behavior).
void Log(LogLevel level, NSString *format, ...)
{
    (void)level;
    va_list args;
    va_start(args, format);
    NSString *message = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);
    fprintf(stderr, "%s\n", message.UTF8String);
}

static void Require(BOOL ok, NSString *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message.UTF8String);
        exit(EXIT_FAILURE);
    }
}

static AVAssetReader *OpenVideo(NSURL *file, AVAssetReaderTrackOutput **output,
                                NSString **problem)
{
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:file options:nil];
    NSArray<AVAssetTrack *> *tracks = asset.tracks;
    AVAssetTrack *video = tracks.firstObject;
    if (tracks.count != 1 || ![video.mediaType isEqualToString:AVMediaTypeVideo]) {
        *problem = [NSString stringWithFormat:@"%@: expected one video track and no other tracks (found %lu tracks)",
                    file.lastPathComponent, (unsigned long)tracks.count];
        return nil;
    }

    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    // nil output settings keep the original encoded H.264 packets.
    AVAssetReaderTrackOutput *encoded = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:video
                                                                                  outputSettings:nil];
    if (reader == nil || ![reader canAddOutput:encoded]) {
        *problem = [NSString stringWithFormat:@"%@: cannot configure encoded reader: %@",
                    file.lastPathComponent, error];
        return nil;
    }
    [reader addOutput:encoded];
    if (![reader startReading]) {
        *problem = [NSString stringWithFormat:@"%@: cannot start reader: %@", file.lastPathComponent, reader.error];
        return nil;
    }
    *output = encoded;
    return reader;
}

static BOOL IsSync(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, NO);
    if (attachments == NULL || CFArrayGetCount(attachments) == 0) {
        return YES;
    }
    CFDictionaryRef first = CFArrayGetValueAtIndex(attachments, 0);
    return CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
}

static NSData *H264Payload(CMSampleBufferRef sample)
{
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (format == NULL || CMFormatDescriptionGetMediaSubType(format) != kCMVideoCodecType_H264 ||
        CMSampleBufferGetNumSamples(sample) != 1 || block == NULL) {
        return nil;
    }
    size_t length = CMBlockBufferGetDataLength(block);
    if (length == 0) {
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:length];
    return CMBlockBufferCopyDataBytes(block, 0, length, data.mutableBytes) == kCMBlockBufferNoErr ? data : nil;
}

static CMTime DecodeTime(CMSampleBufferRef sample)
{
    CMTime dts = CMSampleBufferGetDecodeTimeStamp(sample);
    // No B frames: the container may omit DTS when it equals PTS.
    return CMTIME_IS_VALID(dts) ? dts : CMSampleBufferGetPresentationTimeStamp(sample);
}

static BOOL SameTime(CMTime a, CMTime b)
{
    return CMTIME_IS_NUMERIC(a) && CMTIME_IS_NUMERIC(b) && CMTimeCompare(a, b) == 0;
}

enum { GOPSize = 4 };

typedef struct {
    CMTime pts;
    CMTime decodeOffset;
    CMTime duration;
} PacketTiming;

static NSString *CheckRecording(NSURL *file, NSArray<NSData *> *payloads,
                                const PacketTiming expected[GOPSize])
{
    NSString *problem = nil;
    AVAssetReaderTrackOutput *output = nil;
    AVAssetReader *reader = OpenVideo(file, &output, &problem);
    if (reader == nil) {
        return problem;
    }

    CMTime startPTS = kCMTimeInvalid;
    for (int i = 0; i < GOPSize; ++i) {
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (sample == NULL) {
            return [NSString stringWithFormat:@"missing packet %d (reader status %ld, error %@)",
                    i, (long)reader.status, reader.error];
        }
        BOOL samePayload = [H264Payload(sample) isEqualToData:payloads[i]];
        BOOL sameSync = IsSync(sample) == (i == 0);
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        if (i == 0) {
            startPTS = pts;
        }
        // MOV can rebase the timeline to its first IDR; compare offsets and durations.
        BOOL sameTiming = SameTime(CMTimeSubtract(pts, startPTS),
                                   CMTimeSubtract(expected[i].pts, expected[0].pts)) &&
                          SameTime(CMTimeSubtract(DecodeTime(sample), pts), expected[i].decodeOffset) &&
                          SameTime(CMSampleBufferGetDuration(sample), expected[i].duration);
        CFRelease(sample);
        if (!samePayload || !sameSync || !sameTiming) {
            [reader cancelReading];
            return [NSString stringWithFormat:@"packet %d mismatch (payload=%@, sync=%@, timing=%@)",
                    i, samePayload ? @"OK" : @"BAD", sameSync ? @"OK" : @"BAD", sameTiming ? @"OK" : @"BAD"];
        }
    }
    CMSampleBufferRef extra = [output copyNextSampleBuffer];
    if (extra != NULL) {
        CFRelease(extra);
        [reader cancelReading];
        return @"unexpected extra video packet";
    }
    if (reader.status != AVAssetReaderStatusCompleted) {
        return [NSString stringWithFormat:@"recorded MOV reader did not complete: %@", reader.error];
    }
    return nil;
}

static NSArray<NSURL *> *Entries(NSURL *directory)
{
    NSError *error = nil;
    NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtURL:directory
                                                  includingPropertiesForKeys:nil options:0 error:&error];
    Require(files != nil, [NSString stringWithFormat:@"cannot list %@: %@", directory.path, error]);
    return files;
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        Require(argc == 4, @"usage: smoke INPUT.mov OUTPUT_DIR NO_IDR_DIR");
        NSURL *input = [NSURL fileURLWithFileSystemRepresentation:argv[1] isDirectory:NO relativeToURL:nil];
        NSURL *recordedDir = [NSURL fileURLWithFileSystemRepresentation:argv[2] isDirectory:YES relativeToURL:nil];
        NSURL *noIDRDir = [NSURL fileURLWithFileSystemRepresentation:argv[3] isDirectory:YES relativeToURL:nil];
        Require(Entries(recordedDir).count == 0 && Entries(noIDRDir).count == 0,
                @"test output directories must be empty");

        NSString *problem = nil;
        AVAssetReaderTrackOutput *output = nil;
        AVAssetReader *reader = OpenVideo(input, &output, &problem);
        Require(reader != nil, problem);

        // Keep the recorders alive through asynchronous finish, even under ARC.
        MLH264Recorder *recorder __attribute__((objc_precise_lifetime)) =
            [[MLH264Recorder alloc] initWithDirectory:recordedDir];
        MLH264Recorder *noIDR __attribute__((objc_precise_lifetime)) =
            [[MLH264Recorder alloc] initWithDirectory:noIDRDir];
        Require(recorder != nil && noIDR != nil, @"could not initialize recorders");

        NSMutableArray<NSData *> *payloads = [NSMutableArray arrayWithCapacity:GOPSize];
        PacketTiming expected[GOPSize];
        const int ptsTicks[] = {0, 1, 2, 3, 4, 5, 7, 8};
        for (int i = 0; i < 2 * GOPSize; ++i) {
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            NSData *payload = sample != NULL ? H264Payload(sample) : nil;
            Require(payload != nil,
                    [NSString stringWithFormat:@"fixture: missing encoded H.264 packet %d", i]);
            BOOL keyFrame = IsSync(sample);
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
            Require(keyFrame == (i % GOPSize == 0) &&
                    SameTime(pts, CMTimeMake(ptsTicks[i], 25)),
                    [NSString stringWithFormat:@"fixture: unexpected key frame flag or PTS at packet %d", i]);
            if (i >= GOPSize) {
                [payloads addObject:payload];
                expected[i - GOPSize] = (PacketTiming){pts, CMTimeSubtract(DecodeTime(sample), pts),
                                                       CMSampleBufferGetDuration(sample)};
            }
            if (i > 0) { // Omit the first IDR to emulate starting mid-GOP.
                [recorder appendSample:sample keyFrame:keyFrame];
                if (i < GOPSize) {
                    [noIDR appendSample:sample keyFrame:keyFrame];
                }
            }
            CFRelease(sample);
        }
        CMSampleBufferRef extra = [output copyNextSampleBuffer];
        Require(extra == NULL && reader.status == AVAssetReaderStatusCompleted,
                @"fixture: expected exactly eight readable packets");

        Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file before finish");
        [noIDR finish];
        [recorder finish];

        // finish has no completion callback. Poll for a fully readable MOV and
        // observe the negative directory for at least two seconds after finish.
        NSTimeInterval start = NSProcessInfo.processInfo.systemUptime;
        do {
            @autoreleasepool {
                Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file");
                NSArray<NSURL *> *files = Entries(recordedDir);
                if (files.count == 1 && [files.firstObject.pathExtension.lowercaseString isEqualToString:@"mov"]) {
                    problem = CheckRecording(files.firstObject, payloads, expected);
                } else {
                    problem = [NSString stringWithFormat:@"expected exactly one .mov in the output directory, found %@",
                               [files valueForKey:@"lastPathComponent"]];
                }
                if (problem == nil && NSProcessInfo.processInfo.systemUptime - start >= 2.0) {
                    break;
                }
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
        } while (NSProcessInfo.processInfo.systemUptime - start < 10.0);
        Require(problem == nil, [NSString stringWithFormat:@"recording did not verify before timeout: %@", problem]);
        Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file");

        puts("PASS: video-only MOV preserved H.264 payloads and timing from the first IDR; pre-IDR-only input produced no file");
    }
    return 0;
}
