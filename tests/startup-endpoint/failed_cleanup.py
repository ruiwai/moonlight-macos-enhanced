"""Ensure deferred startup-failure cleanup cannot stop a replacement manager."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'Limelight/macOS/ViewControllers/StreamViewController.m').read_text()


def cleanup(signature):
    method = source[source.index(signature):]
    start = method.index('    StreamManager *failedStreamManager = self.streamMan;')
    end = method.index('\n    }\n', start) + len('\n    }')
    return method[start:end]


program = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
@interface StreamManager : NSObject
@property int stops;
- (void)stopStream;
@end
@implementation StreamManager
- (void)stopStream { self.stops++; }
@end
@interface Stream : NSObject
@property StreamManager *streamMan;
- (void)launchCleanup;
- (void)stageCleanup;
@end
// Deterministically defer the production block until a replacement is installed.
static dispatch_block_t pending;
#define dispatch_async(queue,block) pending=[block copy]
@implementation Stream
- (void)launchCleanup {
''' + cleanup('- (void)launchFailed:') + r'''
}
- (void)stageCleanup {
''' + cleanup('- (void)stageFailed:') + r'''
}
@end
#undef dispatch_async
int main(void) { @autoreleasepool {
 for (int stage=0; stage<2; stage++) {
  Stream *s=[Stream new];StreamManager *old=[StreamManager new],*replacement=[StreamManager new];s.streamMan=old;
  if (stage) [s stageCleanup]; else [s launchCleanup];
  assert(pending!=nil);s.streamMan=replacement;pending();pending=nil;
  assert(old.stops==1 && replacement.stops==0);
 }
 puts("PASS: delayed launch/stage failure cleanup stops only the failed stream manager");
} }
'''
with tempfile.TemporaryDirectory(prefix='moonlight-failed-cleanup-') as tmp:
    path = Path(tmp)
    (path/'test.m').write_text(program)
    subprocess.run(['clang','-fobjc-arc','-fblocks','-framework','Foundation',str(path/'test.m'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
