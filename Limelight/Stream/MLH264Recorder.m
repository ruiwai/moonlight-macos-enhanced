#import "MLH264Recorder.h"
#import "Logger.h"
#import <AVFoundation/AVFoundation.h>
#include <stdatomic.h>

@implementation MLH264Recorder {
    NSURL *_directory;
    dispatch_queue_t _queue;
    dispatch_semaphore_t _slots;
    atomic_bool _closed;
    BOOL _seenKeyFrame; // producer only

    // Accessed only on _queue.
    AVAssetWriter *_writer;
    AVAssetWriterInput *_input;
    NSString *_sessionName;
    NSUInteger _partNumber;
    CMTime _lastDecodeTime;
    BOOL _failed;
}

- (instancetype)initWithDirectory:(NSURL *)directory {
    if ((self = [super init])) {
        _directory = directory;
        _queue = dispatch_queue_create("std.skyhua.moonlight.h264-recorder", DISPATCH_QUEUE_SERIAL);
        _slots = dispatch_semaphore_create(32);
        atomic_init(&_closed, NO);
    }
    return self;
}

- (BOOL)openForSample:(CMSampleBufferRef)sample {
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:_directory withIntermediateDirectories:YES
                                                 attributes:nil error:&error]) {
        Log(LOG_W, @"[recording] Cannot create %@: %@", _directory.path, error);
        return NO;
    }
    if (_sessionName == nil) {
        NSDateFormatter *date = [[NSDateFormatter alloc] init];
        date.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        date.dateFormat = @"yyyy-MM-dd_HH-mm-ss";
        _sessionName = [NSString stringWithFormat:@"Moonlight-%@-%@", [date stringFromDate:NSDate.date], NSUUID.UUID.UUIDString];
    }
    NSString *name = [NSString stringWithFormat:@"%@-part-%04lu.mov", _sessionName, (unsigned long)++_partNumber];
    NSURL *url = [_directory URLByAppendingPathComponent:name];
    _writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeQuickTimeMovie error:&error];
    // nil output settings = mux these compressed samples, do not instantiate an encoder.
    _input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:nil
                                             sourceFormatHint:CMSampleBufferGetFormatDescription(sample)];
    _input.expectsMediaDataInRealTime = YES;
    _input.mediaTimeScale = CMSampleBufferGetPresentationTimeStamp(sample).timescale;
    if (![_writer canAddInput:_input]) {
        Log(LOG_W, @"[recording] Cannot open H.264 track at %@: %@", url.path, error ?: _writer.error);
        return NO;
    }
    [_writer addInput:_input];
    _writer.movieFragmentInterval = CMTimeMake(5, 1);
    if (![_writer startWriting]) {
        Log(LOG_W, @"[recording] Cannot start %@: %@", url.path, _writer.error);
        return NO;
    }
    [_writer startSessionAtSourceTime:CMSampleBufferGetPresentationTimeStamp(sample)];
    Log(LOG_I, @"[recording] Writing H.264 video to %@", url.path);
    return YES;
}

- (void)closeWriter {
    AVAssetWriter *writer = _writer;
    if (writer.status == AVAssetWriterStatusWriting) {
        [_input markAsFinished];
        [writer finishWritingWithCompletionHandler:^{
            if (writer.status == AVAssetWriterStatusCompleted) {
                Log(LOG_I, @"[recording] Saved %@", writer.outputURL.path);
            } else {
                Log(LOG_W, @"[recording] Could not finish %@: %@", writer.outputURL.path, writer.error);
            }
        }];
    } else if (writer.status == AVAssetWriterStatusFailed) {
        Log(LOG_W, @"[recording] Writer failed: %@", writer.error);
    }
    _writer = nil;
    _input = nil;
    _lastDecodeTime = kCMTimeInvalid;
}

- (BOOL)writeSample:(CMSampleBufferRef)sample keyFrame:(BOOL)keyFrame {
    // Use decode order if supplied. The low-latency stream normally has only PTS.
    CMTime decodeTime = CMSampleBufferGetDecodeTimeStamp(sample);
    if (!CMTIME_IS_NUMERIC(decodeTime)) decodeTime = CMSampleBufferGetPresentationTimeStamp(sample);
    BOOL clockReset = CMTIME_IS_NUMERIC(_lastDecodeTime) && CMTIME_IS_NUMERIC(decodeTime) &&
                      CMTimeCompare(decodeTime, _lastDecodeTime) < 0;
    if (_input && (clockReset || !CMFormatDescriptionEqual(_input.sourceFormatHint, CMSampleBufferGetFormatDescription(sample)))) {
        Log(LOG_I, @"[recording] %@; starting a new part at the next keyframe",
            clockReset ? @"Video timestamp moved backwards" : @"H.264 format changed");
        [self closeWriter];
    }
    if (_input == nil) {
        if (!keyFrame) return YES; // Never start a part with a dependent frame.
        if (![self openForSample:sample]) return NO;
    }
    // Allow a brief stall on the writer queue only. Do not wait indefinitely on disconnect.
    for (int retry = 0; retry < 10 && !_input.readyForMoreMediaData && _writer.status == AVAssetWriterStatusWriting; retry++) {
        [NSThread sleepForTimeInterval:0.005];
    }
    if (!_input.readyForMoreMediaData || ![_input appendSampleBuffer:sample]) {
        Log(LOG_W, @"[recording] Writer stalled or failed; ending this recording early: %@", _writer.error);
        return NO;
    }
    _lastDecodeTime = decodeTime;
    return YES;
}

- (void)appendSample:(CMSampleBufferRef)sample keyFrame:(BOOL)keyFrame {
    if (atomic_load(&_closed) || (!_seenKeyFrame && !keyFrame)) return;

    // Never hold up playback or drop just one inter-predicted frame. If the disk falls behind,
    // keep the contiguous prefix, then finish the file after the queued samples drain.
    if (dispatch_semaphore_wait(_slots, DISPATCH_TIME_NOW) != 0) {
        atomic_store(&_closed, YES);
        dispatch_async(_queue, ^{
            Log(LOG_W, @"[recording] Disk queue full; ending this recording early");
            [self closeWriter];
        });
        return;
    }
    _seenKeyFrame = YES;
    CFRetain(sample);
    dispatch_async(_queue, ^{
        if (!self->_failed && ![self writeSample:sample keyFrame:keyFrame]) {
            // A format change is recoverable; an I/O error isn't. Don't let samples
            // already queued behind a failure open another part.
            self->_failed = YES;
            atomic_store(&self->_closed, YES);
            [self closeWriter];
        }
        CFRelease(sample);
        dispatch_semaphore_signal(self->_slots);
    });
}

- (void)finish {
    if (!atomic_exchange(&_closed, YES)) {
        dispatch_async(_queue, ^{ [self closeWriter]; });
    }
}
@end
