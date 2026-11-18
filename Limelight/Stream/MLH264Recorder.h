#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

// One recorder per stream. The caller serializes append and finish; neither waits for disk I/O.
@interface MLH264Recorder : NSObject
- (instancetype)initWithDirectory:(NSURL *)directory;
- (void)appendSample:(CMSampleBufferRef)sample keyFrame:(BOOL)keyFrame;
- (void)finish;
@end
