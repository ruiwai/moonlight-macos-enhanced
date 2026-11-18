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

typedef struct {
    PacketTiming timing[GOPSize];
    CMVideoDimensions dimensions;
} ExpectedGOP;

static NSString *CheckRecording(NSURL *file, NSArray<NSData *> *payloads,
                                const ExpectedGOP *expected)
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
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        CMVideoDimensions dimensions = format != NULL ? CMVideoFormatDescriptionGetDimensions(format)
                                                       : (CMVideoDimensions){0, 0};
        BOOL sameDimensions = dimensions.width == expected->dimensions.width &&
                              dimensions.height == expected->dimensions.height;
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        if (i == 0) {
            startPTS = pts;
        }
        // MOV can rebase the timeline to its first IDR; compare offsets and durations.
        BOOL sameTiming = SameTime(CMTimeSubtract(pts, startPTS),
                                   CMTimeSubtract(expected->timing[i].pts, expected->timing[0].pts)) &&
                          SameTime(CMTimeSubtract(DecodeTime(sample), pts), expected->timing[i].decodeOffset) &&
                          SameTime(CMSampleBufferGetDuration(sample), expected->timing[i].duration);
        CFRelease(sample);
        if (!samePayload || !sameSync || !sameTiming || !sameDimensions) {
            [reader cancelReading];
            return [NSString stringWithFormat:@"packet %d mismatch (payload=%@, sync=%@, timing=%@, dimensions=%@)",
                    i, samePayload ? @"OK" : @"BAD", sameSync ? @"OK" : @"BAD", sameTiming ? @"OK" : @"BAD",
                    sameDimensions ? @"OK" : @"BAD"];
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

static NSString *CheckRecordings(NSURL *directory, NSArray<NSArray<NSData *> *> *payloads,
                                 const ExpectedGOP *expected)
{
    NSArray<NSURL *> *files = Entries(directory);
    if (files.count != payloads.count) {
        return [NSString stringWithFormat:@"%@: expected %lu .mov file(s), found %@", directory.lastPathComponent,
                (unsigned long)payloads.count, [files valueForKey:@"lastPathComponent"]];
    }
    // Parts from one session sort in recording order; don't assume a naming template.
    files = [files sortedArrayUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        return [a.lastPathComponent compare:b.lastPathComponent options:NSLiteralSearch];
    }];
    for (NSUInteger i = 0; i < files.count; ++i) {
        NSURL *file = files[i];
        NSString *problem = [file.pathExtension.lowercaseString isEqualToString:@"mov"]
            ? CheckRecording(file, payloads[i], &expected[i]) : @"expected a .mov file";
        if (problem != nil) {
            return [NSString stringWithFormat:@"%@: %@", file.path, problem];
        }
    }
    return nil;
}

static NSArray<NSData *> *FeedFixture(NSURL *file, MLH264Recorder *midGOP, MLH264Recorder *preIDROnly,
                                      MLH264Recorder *lastGOPOnly, CMTime midGOPTimeOffset, ExpectedGOP *expected)
{
    NSString *problem = nil;
    AVAssetReaderTrackOutput *output = nil;
    AVAssetReader *reader = OpenVideo(file, &output, &problem);
    Require(reader != nil, problem);

    NSMutableArray<NSData *> *payloads = [NSMutableArray arrayWithCapacity:GOPSize];
    const int ptsTicks[] = {0, 1, 2, 3, 4, 5, 7, 8};
    for (int i = 0; i < 2 * GOPSize; ++i) {
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        NSData *payload = sample != NULL ? H264Payload(sample) : nil;
        Require(payload != nil,
                [NSString stringWithFormat:@"%@: missing encoded H.264 packet %d", file.lastPathComponent, i]);
        BOOL keyFrame = IsSync(sample);
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        Require(keyFrame == (i % GOPSize == 0) && SameTime(pts, CMTimeMake(ptsTicks[i], 25)),
                [NSString stringWithFormat:@"%@: unexpected key frame flag or PTS at packet %d", file.lastPathComponent, i]);
        if (i == GOPSize) {
            expected->dimensions = CMVideoFormatDescriptionGetDimensions(CMSampleBufferGetFormatDescription(sample));
        }
        if (i >= GOPSize) {
            [payloads addObject:payload];
            expected->timing[i - GOPSize] = (PacketTiming){pts, CMTimeSubtract(DecodeTime(sample), pts),
                                                           CMSampleBufferGetDuration(sample)};
            [lastGOPOnly appendSample:sample keyFrame:keyFrame];
        } else if (i > 0) {
            [preIDROnly appendSample:sample keyFrame:keyFrame];
        }
        if (i > 0) { // Omit the first IDR to emulate starting mid-GOP.
            if (CMTimeCompare(midGOPTimeOffset, kCMTimeZero) == 0) {
                [midGOP appendSample:sample keyFrame:keyFrame];
            } else {
                CMSampleTimingInfo timing;
                Require(CMSampleBufferGetSampleTimingInfo(sample, 0, &timing) == noErr,
                        @"could not read fixture packet timing");
                timing.presentationTimeStamp = CMTimeAdd(timing.presentationTimeStamp, midGOPTimeOffset);
                // Leave an omitted DTS alone; the recorder then uses the shifted PTS.
                if (CMTIME_IS_NUMERIC(timing.decodeTimeStamp)) {
                    timing.decodeTimeStamp = CMTimeAdd(timing.decodeTimeStamp, midGOPTimeOffset);
                }
                CMSampleBufferRef shifted = NULL;
                Require(CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sample, 1, &timing, &shifted) == noErr &&
                        shifted != NULL, @"could not shift fixture packet timing");
                [midGOP appendSample:shifted keyFrame:keyFrame];
                CFRelease(shifted);
            }
        }
        CFRelease(sample);
    }
    CMSampleBufferRef extra = [output copyNextSampleBuffer];
    Require(extra == NULL && reader.status == AVAssetReaderStatusCompleted,
            [NSString stringWithFormat:@"%@: expected exactly eight readable packets", file.lastPathComponent]);
    return payloads;
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        Require(argc == 6, @"usage: smoke INPUT.mov CHANGED.mov OUTPUT_DIR NO_IDR_DIR ROLLOVER_DIR");
        NSURL *input = [NSURL fileURLWithFileSystemRepresentation:argv[1] isDirectory:NO relativeToURL:nil];
        NSURL *changed = [NSURL fileURLWithFileSystemRepresentation:argv[2] isDirectory:NO relativeToURL:nil];
        NSURL *recordedDir = [NSURL fileURLWithFileSystemRepresentation:argv[3] isDirectory:YES relativeToURL:nil];
        NSURL *noIDRDir = [NSURL fileURLWithFileSystemRepresentation:argv[4] isDirectory:YES relativeToURL:nil];
        NSURL *rolloverDir = [NSURL fileURLWithFileSystemRepresentation:argv[5] isDirectory:YES relativeToURL:nil];
        Require(Entries(recordedDir).count == 0 && Entries(noIDRDir).count == 0 && Entries(rolloverDir).count == 0,
                @"test output directories must be empty");

        // Keep the recorders alive through asynchronous finish, even under ARC.
        MLH264Recorder *recorder __attribute__((objc_precise_lifetime)) =
            [[MLH264Recorder alloc] initWithDirectory:recordedDir];
        MLH264Recorder *noIDR __attribute__((objc_precise_lifetime)) =
            [[MLH264Recorder alloc] initWithDirectory:noIDRDir];
        MLH264Recorder *rollover __attribute__((objc_precise_lifetime)) =
            [[MLH264Recorder alloc] initWithDirectory:rolloverDir];
        Require(recorder != nil && noIDR != nil && rollover != nil, @"could not initialize recorders");

        ExpectedGOP expected[3];
        NSArray<NSData *> *firstGOP = FeedFixture(input, recorder, noIDR, rollover, kCMTimeZero, &expected[0]);
        // Change formats on a P frame without moving backwards in time.
        NSArray<NSData *> *secondGOP = FeedFixture(changed, rollover, nil, nil, CMTimeMake(1, 1), &expected[1]);
        Require(expected[0].dimensions.width != expected[1].dimensions.width ||
                expected[0].dimensions.height != expected[1].dimensions.height,
                @"rollover fixtures must have different resolutions");
        // Replay packets 1..7 as-is: timestamps reset on a P frame, but the format stays the same.
        NSArray<NSData *> *replayedGOP = FeedFixture(changed, rollover, nil, nil, kCMTimeZero, &expected[2]);

        Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file before finish");
        [noIDR finish];
        [recorder finish];
        [rollover finish];

        // finish has no completion callback. Poll for the fully readable MOVs and
        // observe the negative directory for at least two seconds after finish.
        NSString *problem = nil;
        NSTimeInterval start = NSProcessInfo.processInfo.systemUptime;
        do {
            @autoreleasepool {
                Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file");
                problem = CheckRecordings(recordedDir, @[firstGOP], expected);
                if (problem == nil) {
                    problem = CheckRecordings(rolloverDir, @[firstGOP, secondGOP, replayedGOP], expected);
                }
                if (problem == nil && NSProcessInfo.processInfo.systemUptime - start >= 2.0) {
                    break;
                }
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
        } while (NSProcessInfo.processInfo.systemUptime - start < 10.0);
        Require(problem == nil, [NSString stringWithFormat:@"recording did not verify before timeout: %@", problem]);
        Require(Entries(noIDRDir).count == 0, @"pre-IDR-only input created a file");

        puts("PASS: IDR gating and both rollover cases preserved H.264 packets and timing; pre-IDR-only input produced no file");
    }
    return 0;
}
