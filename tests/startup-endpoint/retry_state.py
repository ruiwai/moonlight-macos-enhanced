"""Execute the production error and retry handlers on an isolated main run loop."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


window = (root/'Limelight/macOS/ViewControllers/StreamViewController+WindowModes.m').read_text()
diag = (root/'Limelight/macOS/ViewControllers/StreamViewController+Diagnostics.m').read_text()
program = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#define LOG_W 1
#define Log(...) ((void)0)
#define MLString(key,comment) key
@interface HID : NSObject
- (void)releaseAllModifierKeys;
@end
@implementation HID
- (void)releaseAllModifierKeys {}
@end
@interface Delegate : NSObject
- (void)appDidQuit:(id)app;
@end
@implementation Delegate
- (void)appDidQuit:(id)app {}
@end
@interface Stream : NSObject
@property HID *hidSupport;
@property Delegate *delegate;
@property id app;
@property BOOL reconnectInProgress,stopStreamInProgress;
@property int errors,hiddenReconnects,retries,closes;
- (void)closeWindowFromMainQueueWithMessage:(NSString *)message;
- (void)handleTimeoutReconnect:(id)sender;
@end
@implementation Stream
- (void)tearDownControllerSupportOnMainThreadIfNeeded {}
- (void)uncaptureMouseWithCode:(NSString *)code reason:(NSString *)reason {}
- (void)hideReconnectOverlay { self.hiddenReconnects++; }
- (void)hideConnectionTimeoutOverlay {}
- (void)showErrorOverlayWithTitle:(NSString *)title message:(NSString *)message canWait:(BOOL)wait { self.errors++; }
- (void)requestSafeCloseOfStreamWindow { self.closes++; }
- (void)attemptReconnectWithReason:(NSString *)reason { self.retries++; self.reconnectInProgress=YES; }
''' + method(window, '- (void)closeWindowFromMainQueueWithMessage:') + '\n' + method(diag, '- (void)handleTimeoutReconnect:') + r'''
@end
static void waitFor(Stream *s,int errors,int closes) {
 NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:1];
 while ((s.errors<errors || s.closes<closes) && deadline.timeIntervalSinceNow>0) {
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
 }
 assert(s.errors==errors && s.closes==closes);
}
int main(void) { @autoreleasepool {
 Stream *s=[Stream new];s.hidSupport=[HID new];s.delegate=[Delegate new];
 s.reconnectInProgress=YES;
 [s closeWindowFromMainQueueWithMessage:@"Request timed out"];waitFor(s,1,0);
 assert(!s.reconnectInProgress && s.hiddenReconnects==1);
 [s handleTimeoutReconnect:nil];assert(s.retries==1 && s.reconnectInProgress);
 [s closeWindowFromMainQueueWithMessage:@"Another failure"];waitFor(s,2,0);
 [s handleTimeoutReconnect:nil];assert(s.retries==2 && s.reconnectInProgress);
 // Existing guards still prevent duplicate retries or a retry during shutdown.
 [s handleTimeoutReconnect:nil];assert(s.retries==2);
 s.reconnectInProgress=NO;s.stopStreamInProgress=YES;
 [s handleTimeoutReconnect:nil];assert(s.retries==2);
 s.stopStreamInProgress=NO;
 [s closeWindowFromMainQueueWithMessage:nil];waitFor(s,2,1);
 puts("PASS: failed reconnect clears busy state and allows another retry");
 puts("PASS: duplicate/shutdown retry guards and normal window close are preserved");
} }
'''
with tempfile.TemporaryDirectory(prefix='moonlight-retry-state-') as tmp:
    path = Path(tmp)
    (path/'test.m').write_text(program)
    subprocess.run(['clang','-fobjc-arc','-fblocks','-framework','Foundation',str(path/'test.m'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
