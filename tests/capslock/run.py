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
a=vc.index('- (void)flagsChanged:(NSEvent *)event {');b=vc.index('    NSEventModifierFlags relevantModifiers = ',a)
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
@property BOOL imeEnabled;
@property int inputMethodEvents;
- (void)flagsChanged:(NSEvent *)e;
- (BOOL)onKeyboardEquivalent:(NSEvent *)e;
@end
@implementation TestStream
- (BOOL)onKeyboardEquivalent:(NSEvent *)e { self.forwarded++; return YES; }
- (void)routeFlagsChangedToInputMethod:(NSEvent *)e {
 if (self.imeEnabled && self.hidSupport.shouldSendInputEvents) self.inputMethodEvents++;
}
'''+method+'''
@end
static NSEvent *flags(unsigned short key, NSEventModifierFlags flags) {
 return [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:flags timestamp:1 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:key];
}
int main(void) { @autoreleasepool {
 TestStream *c=[TestStream new]; c.hidSupport=[FakeHID new]; c.hidSupport.shouldSendInputEvents=YES;c.imeEnabled=YES;
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==1 && c.inputMethodEvents==1);
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==1);
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==2);
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==2);
 [c flagsChanged:flags(kVK_Shift,NSEventModifierFlagShift)]; assert(c.otherModifiers==1 && c.forwarded==2);
 c.hidSupport.shouldSendInputEvents=NO;
 [c flagsChanged:flags(kVK_CapsLock,NSEventModifierFlagCapsLock)]; assert(c.forwarded==2);
 assert(c.inputMethodEvents==5); // All captured flags reach the input method, including duplicate Caps edges.
 // Recapture rebases to the current local Caps state, so the next off transition is delivered.
 c.capsLockStateKnown=YES; c.capsLockLastState=YES; c.hidSupport.shouldSendInputEvents=YES;
 [c flagsChanged:flags(kVK_CapsLock,0)]; assert(c.forwarded==3 && c.inputMethodEvents==6);
 // First event may be an OFF transition.
 TestStream *d=[TestStream new];d.hidSupport=[FakeHID new];d.hidSupport.shouldSendInputEvents=YES;
 [d flagsChanged:flags(kVK_CapsLock,0)]; assert(d.forwarded==1);
 puts("PASS: actual Caps Lock routing, duplicate edges, capture gate, recapture, input-method delivery and modifier regression");
} }
'''

def extract_method(source, signature):
 a=source.index(signature);i=source.index('{',a)+1;depth=1
 while depth:
  if source[i]=='{':depth+=1
  elif source[i]=='}':depth-=1
  i+=1
 return source[a:i]
editor=(root/'Limelight/macOS/ViewControllers/SettingsShortcutEditors.swift').read_text()
install=editor[editor.index('  private func installMonitor()',editor.index('private struct KeyboardTranslationRuleEditorSheet:')):]
callback=install[install.index('{ event in')+len('{ event in'):install.index('\n    }')]
handle=extract_method(install,'  private func handle(_ event: NSEvent) -> NSEvent?')
program+='''
struct StreamShortcut { let keyCode: Int; let modifierFlags: NSEvent.ModifierFlags }
enum StreamShortcutProfile {
 static func relevantModifierFlags(_ flags: NSEvent.ModifierFlags) -> NSEvent.ModifierFlags {
  flags.intersection([.control,.option,.shift,.command,.function])
 }
}
class Recorder {
 enum CaptureTarget { case trigger,output }
 var captureTarget: CaptureTarget? = nil
 var triggerShortcut: StreamShortcut? = nil
 var remoteOutputShortcut: StreamShortcut? = nil
 func monitor(_ event:NSEvent)->NSEvent? {
'''+callback+'\n }\n'+handle+'''
}
let recorder=Recorder()
let caps=event(UInt16(kVK_CapsLock),[.capsLock,.control])
let shift=event(UInt16(kVK_Shift),[.shift])
let tab=event(UInt16(kVK_Tab),[],.keyDown)
// No replacement events may escape an idle recorder, including a Caps release.
for e in [caps,shift,tab,event(UInt16(kVK_CapsLock),[])] {
 precondition(recorder.monitor(e) === e)
}
recorder.captureTarget = .trigger
precondition(recorder.monitor(shift) === shift && recorder.captureTarget != nil)
precondition(recorder.monitor(caps) == nil)
precondition(recorder.triggerShortcut?.keyCode == kVK_CapsLock)
precondition(recorder.triggerShortcut?.modifierFlags == [.control])
let release=event(UInt16(kVK_CapsLock),[])
precondition(recorder.monitor(release) === release)
recorder.captureTarget = .output
precondition(recorder.monitor(release) == nil && recorder.remoteOutputShortcut?.keyCode == kVK_CapsLock)
recorder.captureTarget = .trigger
precondition(recorder.monitor(event(UInt16(kVK_Escape),[],.keyDown)) == nil && recorder.captureTarget == nil)
recorder.captureTarget = .trigger
precondition(recorder.monitor(tab) == nil && recorder.triggerShortcut?.keyCode == kVK_Tab)
print("PASS: idle recorder preserves event identity; Caps trigger/output, release, ordinary keys and cancellation work")
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
