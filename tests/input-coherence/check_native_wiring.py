"""Static native wiring guard only; does not establish macOS runtime behavior."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
pointer = (root / 'Limelight/Input/HIDSupport+Pointer.m').read_text()
internal = (root / 'Limelight/Input/HIDSupport_Internal.h').read_text()
core = (root / 'Limelight/Input/HIDSupport.m').read_text()
scroll = (root / 'Limelight/Input/HIDSupport+Scroll.m').read_text()


def body(source, signature):
    start = source.index('{', source.index(signature))
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


for axis in ('X', 'Y'):
    assert f'@property (atomic) CGFloat mouseDelta{axis};' in internal
registration = body(pointer, '-(void)registerMouseCallbacks:')
movement = body(registration, 'mouse.mouseInput.mouseMovedHandler = ^')
assert re.fullmatch(r'\{\s*self.mouseDeltaX \+= deltaX;\s*self.mouseDeltaY -= deltaY;\s*\}', movement)
assert 'static CGFloat const HIDGCMouseRelativeSpeedDivisor = 2.5;' in pointer
assert '[owners setObject:self forKey:mouse]' in registration
removal = body(pointer, '-(void)unregisterMouseCallbacks:')
assert '[owners objectForKey:mouse] != self' in removal
assert 'mouse.mouseInput.mouseMovedHandler = nil;' in removal

display = body(pointer, 'static CVReturn displayLinkOutputCallback')
physical = display[display.index('    CGFloat deltaX, deltaY;'):display.index('    // Mouse Emulation Movement')]
for token in ('deltaX = me.mouseDeltaX;', 'deltaY = me.mouseDeltaY;',
              'me.mouseDeltaX = 0;', 'me.mouseDeltaY = 0;', 'if (me.shouldSendInputEvents)',
              'if (!inputCtx)', 'HIDShouldUseAbsolutePointerPath', 'HIDShouldSuppressRelativeMouse',
              'deltaX / HIDGCMouseRelativeSpeedDivisor', 'deltaY / HIDGCMouseRelativeSpeedDivisor',
              'HIDScaledRelativeDelta(normalizedDeltaX, sensitivity)',
              'HIDScaledRelativeDelta(normalizedDeltaY, sensitivity)', 'suppressed:YES',
              'HIDDispatchInput(me, inputCtx, ^{', 'LiSendMouseMoveEventCtx(inputCtx, moveX, moveY);',
              'Mouse Runtime Path GameController Active', 'Mouse Runtime Detail GameController Active'):
    assert token in physical, token
assert physical.index('me.mouseDeltaY = 0;') < physical.index('if (me.shouldSendInputEvents)')
assert physical.index('LiSendMouseMoveEventCtx') < physical.index('updateMouseInputRuntimeStatusFor')
for token in ('me.controller.isMouseMode && me.shouldSendInputEvents',
              'HIDInputContext(me)', 'me.controller.lastRightStickX',
              'me.controller.lastRightStickY', 'short deadzone = 4000;',
              'float sensitivity = 15.0f;', 'moveY = (short)(-dy);',
              '@"controllerMouse"', 'HIDDispatchInput(me, inputCtx, ^{',
              'LiSendMouseMoveEventCtx(inputCtx, moveX, moveY);'):
    assert token in display, token
initialization = body(pointer, '- (BOOL)initializeDisplayLink')
for token in ('CVDisplayLinkCreateWithCGDisplay', 'CVDisplayLinkSetOutputCallback',
              'displayLinkOutputCallback', 'CVDisplayLinkStart'):
    assert token in initialization, token
cleanup = body(core, '- (void)tearDownHidManagerOnMainThread')
assert 'CVDisplayLinkStop' in cleanup and 'CVDisplayLinkRelease' in cleanup

relative = body(core, '- (void)dispatchRelativeMouseDeltaX:')
for gate in ('!self.shouldSendInputEvents', 'shouldSuppressPointerMotion',
             'HIDValidateInputContext', 'HIDShouldUseAbsolutePointerPath',
             'if (suppressed || (moveX == 0 && moveY == 0))'):
    assert relative.index(gate) < relative.index('HIDDispatchInput'), gate
assert 'HIDScaledRelativeDelta(deltaX, sensitivity)' in relative
assert 'HIDScaledRelativeDelta(deltaY, sensitivity)' in relative
assert 'gcMouse' not in relative and 'updateMouseInputRuntimeStatusFor' not in relative
print('PASS: static baseline physical GC accumulation/display policy and controller display lifecycle')

for button in ('leftButton', 'middleButton', 'rightButton', 'auxiliaryButtons[0]', 'auxiliaryButtons[1]'):
    handler = body(registration, f'mouse.mouseInput.{button}.pressedChangedHandler = ^')
    queued = body(handler, 'HIDDispatchInput(self, inputCtx, ^')
    assert handler.count('HIDDispatchInput(') == 1
    assert handler.count('LiSendMouseButtonEventCtx(') == queued.count('LiSendMouseButtonEventCtx(') == 1
    assert 'Log(LOG_W' in body(queued, 'if (err != 0)')
for signature in ('- (void)handleGCMouseScrollValueY:', '- (void)scrollWheel:'):
    method = body(scroll, signature)
    queued = body(method, 'HIDDispatchInput(self, inputCtx, ^')
    assert method.count('HIDDispatchInput(') == 1
    assert queued.count('LiNoteScrollTraceLocalDispatchCtx(') == 1
    sends = re.findall(r'LiSendHighResH?ScrollEventCtx\([^;]*?\)', queued)
    assert sends and len(sends) == len(re.findall(r'LiSendHighResH?ScrollEventCtx\(', method))
    assert queued.index('LiNoteScrollTraceLocalDispatchCtx(') < queued.index('LiSendHighRes')
    assert not re.search(r'\b(self|event)\b|LiGetMillis|HIDClassify|__block', queued)
    assert 'Log(LOG_W' in body(queued, 'if (err != 0)')
print('PASS: static five GC button wrappers and scroll trace/send queue boundaries (not runtime)')

for signature in ('- (void)keyDown:', '- (void)keyUp:', '- (void)releaseAllModifierKeys {'):
    method = body(core, signature)
    before, queued = method.split('HIDDispatchInput(self, inputCtx, ^{', 1)
    assert '_pressedKeyPairs' not in before
    assert '[_pressedKeyPairsLock lock]' in queued
    assert 'LiSendKeyboardEvent2Ctx' in queued and 'if (err == 0)' in queued
print('PASS: static tracked keyboard state remains queued and success-guarded (not runtime)')
