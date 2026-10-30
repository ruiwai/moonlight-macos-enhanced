"""Extract production lifecycle methods; Foundation fake FIFO/touchpad on macOS, static-only on Linux."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
core, pointer, scroll, internal = [(root / ('Limelight/Input/' + p)).read_text() for p in
    ('HIDSupport.m', 'HIDSupport+Pointer.m', 'HIDSupport+Scroll.m', 'HIDSupport_Internal.h')]

def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

methods = [extract(core, '- (' + sig) for sig in ('uint64_t)inputMotionEpoch',
    'void)resetRelativeMouseMotion', 'uint64_t)inputWheelEpoch', 'void)resetMouseWheel',
    'void)resetPointerInputLifecycle', 'void)dispatchRelativeMouseDeltaX:')]
methods += [extract(pointer, '- (void)' + sig) for sig in
    ('mouseMoved:', 'handleTrackpadTouchesEvent:', 'refreshPrecisionTouchpadConfiguration')]
methods += [extract(scroll, '- (void)scrollWheel:')]
assert '_wheelEpoch' not in methods[1] and 'resetMouseWheel' not in methods[1]
assert '_relativeMotionEpoch' not in methods[3] and '__atomic_add_fetch(&_wheelEpoch, 1,' in methods[3]
assert methods[4].count('[self reset') == 2
assert 'inputMotionEpoch' not in methods[-1] and methods[-1].count('inputWheelEpoch') == 2
assert '[self resetMouseWheel];' in methods[-1].split('PML_INPUT_STREAM_CONTEXT')[0]
assert 'resetPointerInputLifecycle' not in methods[6] and 'resetMouseWheel' not in methods[6]
assert methods[7].count('shouldSuppressPointerMotion') == 2 and 'wasSuppressed !=' in methods[7]
assert 'self.precisionTouchpad.mode != mode' in methods[8]
for sig in ('dispatchRelativeMouseDeltaX:', 'mouseMoved:', 'suppressRelativeMouseMotionForMilliseconds:',
            'resetFreeMouseVirtualCursorState'):
    text = core if sig.startswith('dispatch') else pointer
    body = extract(text, '- (void)' + sig)
    assert 'resetMouseWheel' not in body and 'resetPointerInputLifecycle' not in body
# Native API/diagnostic stubs only; reset/accessors, wrappers, conversion and lazy clearing are unmodified.
diag = []
for sig in ('- (uint64_t)prepareScrollTraceFromSource:', '- (void)recordRelativeInputDiagnosticsFrom:',
            '- (void)recordAbsoluteInputDiagnosticsFrom:', '- (void)recordScrollInputDiagnosticsMode:'):
    start = internal.index(sig)
    diag.append(internal[start:internal.index(';', start)])
ivars = core[core.index('    HIDRelativeMotionResidual _relativeMotionResiduals'):core.index('    // Mac key code')]
program = r'''#import <Foundation/Foundation.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "HIDRelativeMotionPolicy.h"
#include "HIDMouseWheelPolicy.h"
#define Log(...) ((void)0)
typedef void *PML_INPUT_STREAM_CONTEXT;
typedef NSUInteger NSEventPhase;
typedef NSInteger MLPrecisionTouchpadMode;
enum { MLPrecisionTouchpadModeOff, MLPrecisionTouchpadModeBlock, MLPrecisionTouchpadModeBoth };
static uint64_t now = 1000;
static BOOL absolute;
static NSInteger storedMode;
static NSMutableArray *pending, *packets;
@interface NSEvent : NSObject
@property CGFloat deltaX, deltaY;
@property BOOL hasPreciseScrollingDeltas;
@property NSEventPhase phase, momentumPhase;
@property int touchState;
@end
@implementation NSEvent @end
@interface NSView : NSObject @end
@implementation NSView @end
@interface Host : NSObject
@property(copy) NSString *uuid;
@end
@implementation Host @end
@interface Pad : NSObject
@property MLPrecisionTouchpadMode mode;
@property BOOL active, suppressWheel;
@property uint64_t until;
- (BOOL)shouldSuppressPointerMotion;
- (BOOL)shouldSuppressScrollEvent:(NSEvent *)event;
- (void)handleTouchesEvent:(NSEvent *)event inView:(NSView *)view;
- (void)reset;
@end
@implementation Pad
- (BOOL)shouldSuppressPointerMotion { return self.active || now < self.until; }
- (BOOL)shouldSuppressScrollEvent:(NSEvent *)event { return self.suppressWheel; }
- (void)handleTouchesEvent:(NSEvent *)e inView:(NSView *)v {
 self.active=e.touchState==1; self.until=e.touchState==2 ? now+100 : 0;
}
- (void)reset { self.active=NO; self.until=0; }
@end
@interface SettingsClass : NSObject
+ (NSInteger)touchscreenModeFor:(NSString *)uuid;
+ (NSInteger)hostPrecisionTouchpadModeFor:(NSString *)uuid;
+ (BOOL)reverseScrollDirectionFor:(NSString *)uuid;
+ (void)updateMouseInputRuntimeStatusFor:(NSString *)uuid summaryKey:(NSString *)s detailKey:(NSString *)d;
+ (void)updateScrollInputRuntimeStatusFor:(NSString *)uuid summaryKey:(NSString *)s detailKey:(NSString *)d;
@end
@implementation SettingsClass
+ (NSInteger)touchscreenModeFor:(NSString *)uuid { return 0; }
+ (NSInteger)hostPrecisionTouchpadModeFor:(NSString *)uuid { return storedMode; }
+ (BOOL)reverseScrollDirectionFor:(NSString *)uuid { return NO; }
+ (void)updateMouseInputRuntimeStatusFor:(NSString *)u summaryKey:(NSString *)s detailKey:(NSString *)d {}
+ (void)updateScrollInputRuntimeStatusFor:(NSString *)u summaryKey:(NSString *)s detailKey:(NSString *)d {}
@end
@interface Fixture : NSObject { @public
''' + ivars + r'''}
@property BOOL shouldSendInputEvents, useGCMouse, useCoreHIDMouse;
@property void *inputContext;
@property(strong) Host *host;
@property(strong) Pad *precisionTouchpad;
@property(strong) Fixture *coreHIDMouseDriver;
@property double secondsSinceLastMovementEvent;
@property HIDWheelState wheelState;
@property uint64_t wheelQueueEpoch;
''' + '\n'.join(m.split('{', 1)[0] + ';' for m in methods) + '\n' + ';\n'.join(diag) + ';\n@end\n'
program += r'''
static void HIDDispatchInput(Fixture *f, void *ctx, void (^work)(void)) { [pending addObject:[work copy]]; }
static void *HIDInputContext(Fixture *f) { return f.inputContext; }
static BOOL HIDValidateInputContext(void *ctx, const char *tag) { return ctx != NULL; }
static BOOL HIDShouldUseAbsolutePointerPath(Fixture *f, NSInteger mode) { return absolute; }
static BOOL HIDShouldSuppressRelativeMouse(Fixture *f) { return NO; }
static CGFloat HIDPointerSensitivityForHost(Host *host) { return 1; }
static uint64_t LiGetMillis(void) { return now; }
static BOOL HIDAbsoluteMousePayloadForEvent(NSEvent *e, BOOL clamp, short *x, short *y, short *w, short *h) { *x=*y=*w=*h=1; return YES; }
static int LiSendMouseMoveEventCtx(void *ctx, short x, short y) { [packets addObject:@[@"motion", @(x), @(y)]]; return 0; }
static void LiSendMousePositionEventCtx(void *ctx, short x, short y, short w, short h) { [packets addObject:@[@"absolute"]]; }
#define WHEEL_STUB(name, type) static int name(void *ctx, type n) { [packets addObject:@[@"wheel", @(n)]]; return 0; }
WHEEL_STUB(LiSendHighResHScrollEventCtx, short)
WHEEL_STUB(LiSendHighResScrollEventCtx, short)
WHEEL_STUB(LiSendHScrollEventCtx, signed char)
WHEEL_STUB(LiSendScrollEventCtx, signed char)
static void LiNoteScrollTraceLocalDispatchCtx(void *ctx, uint64_t t, uint64_t d, short n, BOOL s, BOOL h) {}
'''
program += 'typedef int (^HIDRelativeChunkSender)(int16_t, int16_t);\n' + extract(core, 'static int HIDSendRelativeChunk(')
program += '\n' + scroll[scroll.index('typedef int (^HIDWheelSubmitter)'):scroll.index('@implementation')]
program += '@implementation Fixture\n' + '\n'.join(d + (' { return 0; }' if d.startswith('- (uint64_t)') else ' {}') for d in diag)
program += '\n' + '\n'.join(methods) + '\n@end\n' + r'''
static void drain(void) { while (pending.count) { void (^b)(void)=pending[0]; [pending removeObjectAtIndex:0]; b(); } }
static Fixture *fresh(void) {
 pending=[NSMutableArray array]; packets=[NSMutableArray array]; absolute=NO; now=1000; storedMode=0;
 Fixture *f=[Fixture new]; f.shouldSendInputEvents=YES; f.inputContext=(void *)1;
 f.precisionTouchpad=[Pad new]; f.host=[Host new]; f.host.uuid=@"fixture"; return f;
}
static void move(Fixture *f, double x) { [f dispatchRelativeMouseDeltaX:x deltaY:0 sourceTag:@"mouseMoved"]; }
int main(void) { @autoreleasepool {
 Fixture *f=fresh(); NSEvent *e=[NSEvent new]; e.hasPreciseScrollingDeltas=YES; e.deltaY=.75;
 [f scrollWheel:e]; uint64_t wheel=[f inputWheelEpoch]; absolute=YES; [f mouseMoved:e]; drain();
 assert(packets.count==2 && [packets[0][1] intValue]==90 && f.wheelState.y==.75f && wheel==[f inputWheelEpoch]);
 e.deltaY=.25; [f scrollWheel:e]; [f mouseMoved:e]; drain(); assert(f.wheelState.y==0 && [packets[2][1] intValue]==30);
 puts("PASS: queued wheel survives ordinary absolute motion; fractional SDL continuity survives too");
 f=fresh(); move(f,.75); drain(); assert(f->_relativeMotionResiduals[2].x==.75); move(f,.5);
 e.touchState=1; [f handleTrackpadTouchesEvent:e inView:nil]; drain(); assert(!packets.count);
 e.touchState=2; [f handleTrackpadTouchesEvent:e inView:nil]; now+=101; move(f,.25); drain();
 assert(!packets.count && f->_relativeMotionResiduals[2].x==.25);
 move(f,.75); e.touchState=1; [f handleTrackpadTouchesEvent:e inView:nil]; drain(); assert(!packets.count);
 e.touchState=3; uint64_t endEpoch=[f inputMotionEpoch]; [f handleTrackpadTouchesEvent:e inView:nil];
 assert(endEpoch!=[f inputMotionEpoch]); drain(); move(f,.25); drain();
 assert(!packets.count && f->_relativeMotionResiduals[2].x==.25);
 puts("PASS: no-relative touch session clears .75; linger expiry cannot revive it; queued motion drops on entry; observable exit advances epoch");
 f=fresh(); e.deltaY=.75; [f scrollWheel:e]; drain(); move(f,.75); drain(); [f scrollWheel:e];
 uint64_t motion=[f inputMotionEpoch]; f.precisionTouchpad.suppressWheel=YES; [f scrollWheel:e]; drain();
 assert(packets.count==1 && motion==[f inputMotionEpoch]); f.precisionTouchpad.suppressWheel=NO;
 e.deltaY=.25; [f scrollWheel:e]; drain(); assert(f.wheelState.y==.25f); move(f,.25); drain(); assert(packets.count==3);
 storedMode=1; wheel=[f inputWheelEpoch]; motion=[f inputMotionEpoch]; [f refreshPrecisionTouchpadConfiguration];
 assert(wheel!=[f inputWheelEpoch] && motion!=[f inputMotionEpoch]); wheel=[f inputWheelEpoch]; motion=[f inputMotionEpoch];
 [f refreshPrecisionTouchpadConfiguration]; assert(wheel==[f inputWheelEpoch] && motion==[f inputMotionEpoch]);
 puts("PASS: suppressed wheel clears only wheel history/backlog; configuration resets only on mode changes");
} }
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emit', type=Path)
args = parser.parse_args()
if args.emit:
    args.emit.write_text(program)
print('PASS: production lifecycle/reset/accessor/wrapper extraction and static isolation guards (not runtime)', flush=True)
if sys.platform != 'darwin':
    print('SKIP: lifecycle Foundation runtime requires macOS; not executed on ' + sys.platform)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix='input-lifecycle-') as tmp:
    path = Path(tmp)
    (path / 'lifecycle.m').write_text(program)
    subprocess.run(['clang', '-fobjc-arc', '-fblocks', '-Wall', '-Werror', '-fsanitize=undefined,float-cast-overflow',
                    '-framework', 'Foundation', '-I' + str(root / 'Limelight/Input'),
                    str(path / 'lifecycle.m'), '-o', str(path / 'lifecycle')], check=True, timeout=60)
    subprocess.run([str(path / 'lifecycle')], check=True, timeout=30)
