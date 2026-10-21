from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'Limelight/macOS/ViewControllers/SettingsShortcuts.swift').read_text()
start=s.index('@objcMembers\nfinal class CapsLockKeyEvent:')
end=s.index('\n@objcMembers\nfinal class StreamShortcut:',start)
helper=s[start:end]
program='''import AppKit
import Carbon.HIToolbox
'''+helper+'''
func event(_ code: UInt16, _ flags: NSEvent.ModifierFlags, _ type: NSEvent.EventType = .flagsChanged) -> NSEvent {
 NSEvent.keyEvent(with: type, location: .zero, modifierFlags: flags, timestamp: 1,
 windowNumber: 0, context: nil, characters: "", charactersIgnoringModifiers: "",
 isARepeat: false, keyCode: code)!
}
for flags: NSEvent.ModifierFlags in [[], [.capsLock], [.capsLock,.control], [.shift]] {
 let e=event(UInt16(kVK_CapsLock), flags)
 let press=CapsLockKeyEvent.keyPress(from:e)!
 precondition(press.type == .keyDown && press.keyCode == UInt16(kVK_CapsLock))
 precondition(press.modifierFlags == flags && !press.isARepeat)
}
precondition(CapsLockKeyEvent.keyPress(from:event(UInt16(kVK_Shift), [.shift])) == nil)
precondition(CapsLockKeyEvent.keyPress(from:event(UInt16(kVK_Tab), [], .keyDown)) == nil)
print("PASS: actual Caps Lock event normalization, on/off, chords and unrelated keys")
'''
vc=(root/'Limelight/macOS/ViewControllers/StreamViewController+MouseCapture.m').read_text()
a=vc.index('- (void)flagsChanged:(NSEvent *)event {');b=vc.index('    [self routeFlagsChangedToInputMethod:event];',a)
method=vc[a:b]+ '    self.otherModifiers++;\n}\n'
native='''#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <assert.h>
@interface CapsLockKeyEvent : NSObject
+ (NSEvent *)keyPressFrom:(NSEvent *)e;
@end
@implementation CapsLockKeyEvent
+ (NSEvent *)keyPressFrom:(NSEvent *)e { return e; }
@end
@interface FakeHID : NSObject
@property BOOL shouldSendInputEvents;
@end
@implementation FakeHID
@end
@interface TestStream : NSObject
@property BOOL capsLockStateKnown;
@property BOOL capsLockLastState;
@property FakeHID *hidSupport;
@property int forwarded;
@property int otherModifiers;
- (void)flagsChanged:(NSEvent *)e;
- (BOOL)onKeyboardEquivalent:(NSEvent *)e;
@end
@implementation TestStream
- (BOOL)onKeyboardEquivalent:(NSEvent *)e { self.forwarded++; return YES; }
'''+method+'''
@end
static NSEvent *flags(unsigned short key, NSEventModifierFlags flags) {
 return [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:flags timestamp:1 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:key];
}
int main(void) { @autoreleasepool {
 TestStream *c=[TestStream new]; c.hidSupport=[FakeHID new]; c.hidSupport.shouldSendInputEvents=YES;
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==1);
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==1);
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==2);
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==2);
 [c flagsChanged:flags(kVK_Shift,NSEventModifierFlagShift)]; assert(c.otherModifiers==1 && c.forwarded==2);
 c.hidSupport.shouldSendInputEvents=NO;
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==2);
 // Recapture rebases to the current local Caps state, so the next off transition is delivered.
 c.capsLockStateKnown=YES; c.capsLockLastState=YES; c.hidSupport.shouldSendInputEvents=YES;
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==3);
 // First event may be an OFF transition.
 TestStream *d=[TestStream new];d.hidSupport=[FakeHID new];d.hidSupport.shouldSendInputEvents=YES;
 [d flagsChanged:flags(kVK_CapsLock,0)]; assert(d.forwarded==1);
 puts("PASS: actual Caps Lock routing, duplicate edges, capture gate, recapture and modifier regression");
} }
'''
with tempfile.TemporaryDirectory(prefix='capslock-tests-') as tmp:
 p=Path(tmp);(p/'test.swift').write_text(program);(p/'test.m').write_text(native)
 subprocess.run(['swift','-module-cache-path','/tmp/moonlight-caps-test-cache',str(p/'test.swift')],check=True)
 subprocess.run(['clang','-fobjc-arc','-framework','AppKit','-framework','Carbon',str(p/'test.m'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
# The recorder must accept flagsChanged only for Caps, and Caps must have a supported display key.
editor=(root/'Limelight/macOS/ViewControllers/SettingsShortcutEditors.swift').read_text()
assert 'guard let press = CapsLockKeyEvent.keyPress(from: event) else { return event }' in editor
assert 'kVK_CapsLock: "Caps Lock"' in s
print('PASS: translation recorder normalization and Caps Lock label wired')
