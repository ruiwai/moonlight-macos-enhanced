"""Extract production keyboard methods; execute a queued Foundation fake on macOS only."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'Limelight/Input/HIDSupport.m').read_text()


def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


signatures = ['- (void)keyDown:', '- (void)keyUp:', '- (void)releaseAllModifierKeys {']
methods = [extract(s) for s in signatures]
for method, limit in zip(methods, (70, 70, 60)):
    assert len(method.splitlines()) <= limit
    before, queued = method.split('HIDDispatchInput(self, inputCtx, ^{', 1)
    assert '_pressedKeyPairs' not in before
    assert '[_pressedKeyPairsLock lock]' in queued
    assert 'LiSendKeyboardEvent2Ctx' in queued and 'if (err == 0)' in queued
helpers = '\n'.join(extract(s) for s in ('static inline uint32_t HIDPackKeyPair(',
                                      'static inline short HIDKeyPairKeyCode(',
                                      'static inline char HIDKeyPairFlags('))
program = r'''#import <Foundation/Foundation.h>
#include <assert.h>
typedef void *PML_INPUT_STREAM_CONTEXT;
enum { KEY_ACTION_DOWN = 3, KEY_ACTION_UP = 4, LOG_W = 2 };
static NSMutableArray *pending, *calls, *outcomes, *modifierCalls;
static NSLock *heldLock;
static BOOL running;
static int logs;
@interface NSEvent : NSObject
@property unsigned short keyCode;
@property BOOL isARepeat;
@end
@implementation NSEvent
@end
@interface Fixture : NSObject {
@public
    NSMutableDictionary<NSNumber *, NSNumber *> *_pressedKeyPairs;
    NSLock *_pressedKeyPairsLock;
}
@property BOOL shouldSendInputEvents;
@property BOOL hasContext;
@property uint32_t mappedPair;
@property NSUInteger keyboardPhysicalModifierSourceMask;
@property NSUInteger keyboardRemoteModifierMask;
@property NSUInteger keyboardDeferredShortcutTranslationCommandMask;
- (void)syncKeyboardModifierStateForEvent:(NSEvent *)event;
- (char)translateKeyModifierWithEvent:(NSEvent *)event;
- (uint32_t)keyPairForMacKeyCode:(unsigned short)key;
- (void)keyDown:(NSEvent *)event;
- (void)keyUp:(NSEvent *)event;
- (void)releaseAllModifierKeys;
@end
static PML_INPUT_STREAM_CONTEXT HIDInputContext(Fixture *f) { return f.hasContext ? (void *)1 : NULL; }
static BOOL HIDValidateInputContext(PML_INPUT_STREAM_CONTEXT ctx, const char *op) { return ctx != NULL; }
static void HIDDispatchInput(Fixture *f, PML_INPUT_STREAM_CONTEXT ctx, void (^block)(void)) {
    assert(!running && ctx); [pending addObject:[block copy]];
}
static void Log(int level, NSString *format, ...) {
    assert(running && [heldLock tryLock]); [heldLock unlock]; logs++;
}
static int LiSendKeyboardEvent2Ctx(PML_INPUT_STREAM_CONTEXT ctx, short code, char action, char mods, char flags) {
    assert(running && ctx && ![heldLock tryLock]); // Every tracked send holds the lock.
    int result = outcomes.count ? [outcomes[0] intValue] : 0;
    if (outcomes.count) [outcomes removeObjectAtIndex:0];
    [calls addObject:@[@((uint16_t)code), @(action), @(mods), @((uint8_t)flags), @(result)]];
    return result;
}
static int LiSendKeyboardEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short code, char action, char mods) {
    assert(running && ctx); [modifierCalls addObject:@[@(code), @(action), @(mods)]]; return 0;
}
''' + helpers + r'''
@implementation Fixture
- (instancetype)init {
    if ((self = [super init])) {
        _pressedKeyPairs = [NSMutableDictionary dictionary]; _pressedKeyPairsLock = [NSLock new];
        self.shouldSendInputEvents = YES; self.hasContext = YES;
    } return self;
}
- (void)syncKeyboardModifierStateForEvent:(NSEvent *)event {}
- (char)translateKeyModifierWithEvent:(NSEvent *)event { return 5; }
- (uint32_t)keyPairForMacKeyCode:(unsigned short)key { assert(!running); return self.mappedPair; }
''' + '\n'.join(methods) + r'''
@end
static void step(void) {
    assert(pending.count); void (^b)(void) = pending[0]; [pending removeObjectAtIndex:0];
    running = YES; b(); running = NO;
}
static Fixture *fresh(void) {
    assert(!pending.count); calls = [NSMutableArray array]; outcomes = [NSMutableArray array];
    modifierCalls = [NSMutableArray array]; logs = 0;
    Fixture *f = [Fixture new]; heldLock = f->_pressedKeyPairsLock; return f;
}
static void expect(NSUInteger i, uint32_t pair, int action, int mods, int result) {
    NSArray *want = @[@((uint16_t)HIDKeyPairKeyCode(pair)), @(action), @(mods),
                      @((uint8_t)HIDKeyPairFlags(pair)), @(result)];
    assert([calls[i] isEqual:want]);
}
static void held(Fixture *f, NSNumber *key, uint32_t pair) {
    assert([f->_pressedKeyPairs[key] unsignedIntValue] == pair);
}
int main(void) { @autoreleasepool {
    pending = [NSMutableArray array];
    uint32_t a = HIDPackKeyPair((short)0x8041, 1), b = HIDPackKeyPair((short)0x8042, 0);
    NSEvent *e = [NSEvent new]; e.keyCode = 12;
    Fixture *f = fresh(); f.mappedPair = a;
    [outcomes addObject:@(-1)]; [f keyDown:e]; assert(!f->_pressedKeyPairs.count && !calls.count);
    step(); assert(!f->_pressedKeyPairs.count && logs == 1); expect(0, a, 3, 5, -1);
    puts("PASS: failed down is not tracked");

    f = fresh(); f.mappedPair = a; [f keyDown:e]; f.mappedPair = b; [f keyUp:e];
    assert(!f->_pressedKeyPairs.count && !calls.count && pending.count == 2);
    step(); held(f, @12, a); step(); expect(0, a, 3, 5, 0); expect(1, a, 4, 5, 0);
    assert(!f->_pressedKeyPairs.count); puts("PASS: queued down/up uses successful down pair");

    f = fresh(); f.mappedPair = a; [f keyDown:e]; step();
    f.mappedPair = b; [outcomes addObject:@(-3)]; [f keyDown:e]; step();
    assert(calls.count == 2 && logs == 1); held(f, @12, a); expect(1, a, 4, 0, -3);
    [outcomes addObjectsFromArray:@[@0, @(-1)]]; [f keyDown:e]; step();
    expect(2, a, 4, 0, 0); expect(3, b, 3, 5, -1); assert(!f->_pressedKeyPairs.count);
    [f keyDown:e]; step(); held(f, @12, b);
    f.mappedPair = a; [f keyDown:e]; step();
    expect(5, b, 4, 0, 0); expect(6, a, 3, 5, 0); held(f, @12, a);
    puts("PASS: changed pair aborts on failed release; successful release + failed/new down coherence");

    f = fresh(); f.mappedPair = a; [f keyDown:e]; step(); f.mappedPair = b;
    e.isARepeat = YES; [f keyDown:e]; e.isARepeat = NO; step();
    expect(1, a, 3, 5, 0); held(f, @12, a);
    e.isARepeat = YES; [outcomes addObject:@(-1)]; [f keyDown:e]; step();
    held(f, @12, a); e.isARepeat = NO; assert(logs == 1);
    puts("PASS: captured repeat uses original pair; failed repeat retains successful press");

    f = fresh(); f.mappedPair = a; [f keyDown:e]; step();
    [outcomes addObject:@(-4)]; [f keyUp:e]; step(); held(f, @12, a);
    [outcomes addObject:@(-4)]; [f releaseAllModifierKeys]; step(); held(f, @12, a);
    [f releaseAllModifierKeys]; assert(f->_pressedKeyPairs.count == 1); step();
    assert(!f->_pressedKeyPairs.count && logs == 2 && modifierCalls.count == 16);
    expect(1, a, 4, 5, -4); expect(2, a, 4, 0, -4); expect(3, a, 4, 0, 0);
    NSArray *codes = @[@0x5B, @0x5C, @0xA0, @0xA1, @0xA2, @0xA3, @0xA4, @0xA5];
    for (NSUInteger i = 0; i < 16; i++) {
        NSArray *want = @[codes[i % 8], @4, @0]; assert([modifierCalls[i] isEqual:want]);
    }
    puts("PASS: failed up/cleanup remain retryable; explicit modifier releases unchanged");

    f = fresh(); f.mappedPair = a; [f keyDown:e]; step();
    e.keyCode = 13; f.mappedPair = b; [f keyDown:e]; step(); e.keyCode = 12;
    [outcomes addObjectsFromArray:@[@(-4), @0]]; [f releaseAllModifierKeys]; step();
    assert(f->_pressedKeyPairs.count == 1 && logs == 1 && calls.count == 4);
    uint32_t retained = HIDPackKeyPair([calls[2][0] shortValue], [calls[2][3] unsignedCharValue]);
    assert([f->_pressedKeyPairs.allValues[0] unsignedIntValue] == retained);
    [f releaseAllModifierKeys]; step(); assert(!f->_pressedKeyPairs.count);
    puts("PASS: partial cleanup removes only the successfully released mac key");

    f = fresh(); f.mappedPair = a; [f keyDown:e];
    f.keyboardPhysicalModifierSourceMask = f.keyboardRemoteModifierMask = 7;
    f.keyboardDeferredShortcutTranslationCommandMask = 7; [f releaseAllModifierKeys];
    assert(!f.keyboardPhysicalModifierSourceMask && !f.keyboardRemoteModifierMask);
    assert(!f.keyboardDeferredShortcutTranslationCommandMask && !calls.count);
    step(); held(f, @12, a); step(); expect(1, a, 4, 0, 0); assert(!f->_pressedKeyPairs.count);
    [f releaseAllModifierKeys]; f.mappedPair = b; [f keyDown:e];
    step(); assert(calls.count == 2); step(); held(f, @12, b);
    puts("PASS: cleanup follows queued press and does not swallow a later queued press");

    f = fresh(); f.mappedPair = b; [f keyUp:e]; f.mappedPair = a; step(); expect(0, b, 4, 5, 0);
    f.shouldSendInputEvents = NO; [f keyDown:e]; [f keyUp:e]; assert(!pending.count);
    f.shouldSendInputEvents = YES; f.hasContext = NO;
    [f keyDown:e]; [f keyUp:e]; [f releaseAllModifierKeys]; assert(!pending.count);
    puts("PASS: captured fallback mapping, capture gate and missing context");
} }
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emit', type=Path, help='retain extracted Objective-C fixture even on Linux')
args = parser.parse_args()
if args.emit:
    args.emit.write_text(program)
print('PASS: production method/helper extraction and LOC/queue guards (not runtime)', flush=True)
if sys.platform != 'darwin':
    print('SKIP: keyboard runtime fixture requires macOS Foundation; not executed on ' + sys.platform)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix='keyboard-coherence-') as tmp:
    path = Path(tmp)
    (path / 'keyboard.m').write_text(program)
    subprocess.run(['clang', '-fobjc-arc', '-fblocks', '-Wall', '-Werror', '-framework', 'Foundation',
                    str(path / 'keyboard.m'), '-o', str(path / 'keyboard')], check=True, timeout=60)
    subprocess.run([str(path / 'keyboard')], check=True, timeout=30)
