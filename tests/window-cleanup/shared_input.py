"""Extract GC callbacks; macOS Foundation fake, not device/conversion runtime."""
from pathlib import Path
import argparse
import sys
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'Limelight/Input/HIDSupport+Pointer.m').read_text()

def method(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

owner_helper = method('static NSMapTable<GCMouse *, HIDSupport *> *HIDGCMouseCallbackOwners')

fixture = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#define LOG_W 1
#define Log(...) ((void)0)
#define kCVReturnSuccess 0
#define BUTTON_LEFT 1
#define BUTTON_MIDDLE 2
#define BUTTON_RIGHT 3
#define BUTTON_X1 4
#define BUTTON_X2 5
#define BUTTON_ACTION_PRESS 1
#define BUTTON_ACTION_RELEASE 0
typedef void *PML_INPUT_STREAM_CONTEXT;
@interface GCControllerButtonInput : NSObject
@property(copy) void (^pressedChangedHandler)(GCControllerButtonInput *, float, BOOL);
@end
@implementation GCControllerButtonInput @end
@interface GCControllerAxisInput : NSObject
@property(copy) void (^valueChangedHandler)(GCControllerAxisInput *, float);
@end
@implementation GCControllerAxisInput @end
@interface Scroll : NSObject
@property(copy) void (^valueChangedHandler)(void);
@property GCControllerAxisInput *xAxis, *yAxis;
@end
@implementation Scroll @end
@interface GCMouseInput : NSObject
@property(copy) void (^mouseMovedHandler)(GCMouseInput *, float, float);
@property GCControllerButtonInput *leftButton, *middleButton, *rightButton;
@property NSArray<GCControllerButtonInput *> *auxiliaryButtons;
@property Scroll *scroll;
@end
@implementation GCMouseInput @end
@interface GCMouse : NSObject
@property GCMouseInput *mouseInput;
@end
@implementation GCMouse @end
@interface Host : NSObject
@property NSString *uuid;
@end
@implementation Host @end
static int runtimeUpdates;
@interface SettingsClass : NSObject
+ (NSInteger)touchscreenModeFor:(NSString *)uuid;
+ (void)updateMouseInputRuntimeStatusFor:(NSString *)uuid summaryKey:(NSString *)summary detailKey:(NSString *)detail;
@end
@implementation SettingsClass
+ (NSInteger)touchscreenModeFor:(NSString *)uuid { return 0; }
+ (void)updateMouseInputRuntimeStatusFor:(NSString *)uuid summaryKey:(NSString *)summary detailKey:(NSString *)detail {
 assert([summary isEqualToString:@"Mouse Runtime Path GameController Active"]);
 assert([detail isEqualToString:@"Mouse Runtime Detail GameController Active"]); runtimeUpdates++;
}
@end
@interface Touchpad : NSObject
- (BOOL)consumeMouseButton:(int)button pressed:(BOOL)pressed;
@end
@implementation Touchpad
- (BOOL)consumeMouseButton:(int)button pressed:(BOOL)pressed { return NO; }
@end
@interface HIDSupport : NSObject
@property BOOL useGCMouse, shouldSendInputEvents;
@property NSMutableArray *reports;
@property Host *host;
@property Touchpad *precisionTouchpad;
- (void)resetPointerInputLifecycle;
- (void)dispatchRelativeMouseDeltaX:(CGFloat)x deltaY:(CGFloat)y sourceTag:(NSString *)source;
- (void)recordRelativeInputDiagnosticsFrom:(NSString *)source rawDeltaX:(CGFloat)x rawDeltaY:(CGFloat)y sentDeltaX:(short)sx sentDeltaY:(short)sy suppressed:(BOOL)suppressed;
- (void)registerMouseCallbacks:(GCMouse *)mouse;
- (void)unregisterMouseCallbacks:(GCMouse *)mouse;
@end
static int buttonEvents;
static BOOL deferInput;
static NSMutableArray *pending, *inputEvents;
static PML_INPUT_STREAM_CONTEXT HIDInputContext(HIDSupport *support) { return (__bridge void *)support; }
static int LiSendMouseButtonEventCtx(PML_INPUT_STREAM_CONTEXT ctx, int action, int button) {
 buttonEvents++; [inputEvents addObject:@[@(button), @(action)]]; return 0;
}
static void HIDDispatchInput(HIDSupport *s, PML_INPUT_STREAM_CONTEXT ctx, void (^work)(void)) {
 if (deferInput) { [pending addObject:[work copy]]; } else { work(); }
}
static BOOL HIDShouldUseAbsolutePointerPath(HIDSupport *s, NSInteger mode) { return NO; }
static BOOL HIDShouldSuppressRelativeMouse(HIDSupport *s) { return NO; }
static CGFloat HIDPointerSensitivityForHost(Host *host) { return 1.5; }
static void LiSendMouseMoveEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short x, short y) {
 [((__bridge HIDSupport *)ctx).reports addObject:@[@(x), @(y)]];
}
'''
fixture += source[source.index('static CGFloat const HIDGCMouseRelativeSpeedDivisor'):].split('\n', 1)[0] + '\n'
implementation = owner_helper + '\n@implementation HIDSupport\n' + r'''
- (void)resetPointerInputLifecycle {}
- (void)dispatchRelativeMouseDeltaX:(CGFloat)x deltaY:(CGFloat)y sourceTag:(NSString *)source {
 assert([source isEqualToString:@"gcMouse"]);
 if (!self.shouldSendInputEvents) return;
 [self.reports addObject:@[@(x), @(y)]];
 HIDDispatchInput(self, HIDInputContext(self), ^{ [inputEvents addObject:@[@"move", @(x), @(y)]]; });
}
- (void)recordRelativeInputDiagnosticsFrom:(NSString *)source rawDeltaX:(CGFloat)x rawDeltaY:(CGFloat)y sentDeltaX:(short)sx sentDeltaY:(short)sy suppressed:(BOOL)suppressed {}
''' + method('-(void)registerMouseCallbacks:') + '\n' + method('-(void)unregisterMouseCallbacks:') + '\n@end\n'
checks = r'''
static GCMouse *device(void) {
 GCMouse *mouse=[GCMouse new]; mouse.mouseInput=[GCMouseInput new];
 mouse.mouseInput.leftButton=[GCControllerButtonInput new];
 mouse.mouseInput.middleButton=[GCControllerButtonInput new];
 mouse.mouseInput.rightButton=[GCControllerButtonInput new];
 mouse.mouseInput.auxiliaryButtons=@[[GCControllerButtonInput new],[GCControllerButtonInput new]];
 mouse.mouseInput.scroll=[Scroll new];
 mouse.mouseInput.scroll.xAxis=[GCControllerAxisInput new]; mouse.mouseInput.scroll.yAxis=[GCControllerAxisInput new];
 mouse.mouseInput.scroll.valueChangedHandler=^{};
 mouse.mouseInput.scroll.xAxis.valueChangedHandler=^(GCControllerAxisInput *axis, float v) { assert(0); };
 mouse.mouseInput.scroll.yAxis.valueChangedHandler=^(GCControllerAxisInput *axis, float v) { assert(0); };
 return mouse;
}
static HIDSupport *support(void) {
 HIDSupport *s=[HIDSupport new]; s.useGCMouse=YES; s.shouldSendInputEvents=YES; s.reports=[NSMutableArray array]; return s;
}
static void assertHandlers(GCMouse *mouse, BOOL present) {
 assert((mouse.mouseInput.mouseMovedHandler != nil)==present);
 assert((mouse.mouseInput.leftButton.pressedChangedHandler != nil)==present);
 assert((mouse.mouseInput.middleButton.pressedChangedHandler != nil)==present);
 assert((mouse.mouseInput.rightButton.pressedChangedHandler != nil)==present);
 for (GCControllerButtonInput *button in mouse.mouseInput.auxiliaryButtons) {
  assert((button.pressedChangedHandler != nil)==present);
 }
 assert(mouse.mouseInput.scroll.valueChangedHandler == nil);
 assert(mouse.mouseInput.scroll.xAxis.valueChangedHandler == nil);
 assert(mouse.mouseInput.scroll.yAxis.valueChangedHandler == nil);
}
int main(void) { @autoreleasepool {
 GCMouse *shared=device(), *oldOnly=device();
 HIDSupport *old=support(), *current=support();
 [old registerMouseCallbacks:shared]; [old registerMouseCallbacks:oldOnly];
 [current registerMouseCallbacks:shared];
 // Late teardown of the previous stream must preserve the replacement's callbacks.
 [old unregisterMouseCallbacks:shared]; assertHandlers(shared, YES);
 shared.mouseInput.mouseMovedHandler(shared.mouseInput, 3, 4);
 shared.mouseInput.leftButton.pressedChangedHandler(shared.mouseInput.leftButton, 1, YES);
 assert(buttonEvents==1 && current.reports.count==1);
 assert(!old.reports.count && runtimeUpdates==0);
 assert(([current.reports[0] isEqualToArray:@[@1.2, @-1.6]]));
 // Every tiny report and reversal reaches the relative dispatcher immediately.
 for (int i=0; i<4; i++) shared.mouseInput.mouseMovedHandler(shared.mouseInput, .25, -.25);
 assert(current.reports.count==5);
 for (int i=1; i<5; i++) assert(([current.reports[i] isEqualToArray:@[@0.1, @0.1]]));
 shared.mouseInput.mouseMovedHandler(shared.mouseInput, 10, 5);
 shared.mouseInput.mouseMovedHandler(shared.mouseInput, -10, -5);
 assert(current.reports.count==7);
 assert(([current.reports[5] isEqualToArray:@[@4, @-2]]));
 assert(([current.reports[6] isEqualToArray:@[@-4, @2]]));
 current.shouldSendInputEvents=NO; shared.mouseInput.mouseMovedHandler(shared.mouseInput, 3, 4);
 assert(current.reports.count==7);
 current.shouldSendInputEvents=YES;
 // Callback snapshots enter the same modeled FIFO before each button pair.
 pending=[NSMutableArray array]; inputEvents=[NSMutableArray array]; deferInput=YES;
 NSArray *buttons=@[shared.mouseInput.leftButton, shared.mouseInput.middleButton,
                    shared.mouseInput.rightButton, shared.mouseInput.auxiliaryButtons[0],
                    shared.mouseInput.auxiliaryButtons[1]];
 for (GCControllerButtonInput *button in buttons) {
  shared.mouseInput.mouseMovedHandler(shared.mouseInput, 1, 2);
  button.pressedChangedHandler(button, 1, YES); button.pressedChangedHandler(button, 0, NO);
 }
 assert(pending.count==15 && !inputEvents.count && buttonEvents==1 && current.reports.count==12);
 current.shouldSendInputEvents=NO; // Already accepted button sends remain queued.
 while (pending.count) { void (^work)(void)=pending[0]; [pending removeObjectAtIndex:0]; work(); }
 for (NSUInteger i=0; i<5; i++) {
  assert(([inputEvents[3*i] isEqualToArray:@[@"move", @0.4, @-0.8]]));
  assert(([inputEvents[3*i+1] isEqualToArray:@[@(i+1), @1]]));
  assert(([inputEvents[3*i+2] isEqualToArray:@[@(i+1), @0]]));
 }
 assert(buttonEvents==11 && !pending.count && current.reports.count==12);
 current.shouldSendInputEvents=YES; deferInput=NO;
 puts("PASS: five deferred GC button FIFOs with immediate normalized motion snapshots (dispatcher modeled)");
 [old unregisterMouseCallbacks:oldOnly]; assertHandlers(oldOnly, NO);
 [current unregisterMouseCallbacks:shared]; assertHandlers(shared, NO);
 [current unregisterMouseCallbacks:shared]; assertHandlers(shared, NO);
 // Changing driver and registering again must still work for the current owner.
 [current registerMouseCallbacks:shared]; current.useGCMouse=NO;
 [current registerMouseCallbacks:shared]; assertHandlers(shared, NO);
 [old unregisterMouseCallbacks:shared];
 current.useGCMouse=YES; [current registerMouseCallbacks:shared];
 [old unregisterMouseCallbacks:shared]; assertHandlers(shared, YES);
 [current unregisterMouseCallbacks:shared];
 __weak HIDSupport *released;
 @autoreleasepool {
  HIDSupport *temporary=support(); released=temporary;
  [temporary registerMouseCallbacks:shared]; [temporary unregisterMouseCallbacks:shared];
 }
 assert(released==nil);
 puts("PASS: stale mouse cleanup preserves another stream's movement/buttons; GC wheel stays disabled");
 puts("PASS: owned cleanup, repeated cleanup, driver refresh and owner lifetime");
} }
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emit', type=Path, help='retain extracted fake Objective-C even on Linux')
args = parser.parse_args()
program = fixture + implementation + checks
if args.emit:
    args.emit.write_text(program)
print('PASS: production GC callback extraction (not runtime)', flush=True)
if sys.platform != 'darwin':
    print('SKIP: shared-input fake runtime requires macOS Foundation; not executed on ' + sys.platform)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix='moonlight-shared-input-') as tmp:
    path = Path(tmp)
    (path / 'test.m').write_text(program)
    subprocess.run(['clang', '-fobjc-arc', '-fblocks', '-framework', 'Foundation',
                    str(path / 'test.m'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
