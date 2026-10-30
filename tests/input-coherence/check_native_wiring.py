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


assert 'mouseDeltaX' not in internal and 'mouseDeltaY' not in internal
registration = body(pointer, '-(void)registerMouseCallbacks:')
movement = body(registration, 'mouse.mouseInput.mouseMovedHandler = ^')
for token in ('dispatchRelativeMouseDeltaX:deltaX / HIDGCMouseRelativeSpeedDivisor',
              'deltaY:-deltaY / HIDGCMouseRelativeSpeedDivisor', 'sourceTag:@"gcMouse"'):
    assert token in movement, token
assert 'dispatch_async' not in movement and 'mouseDelta' not in movement
assert 'static CGFloat const HIDGCMouseRelativeSpeedDivisor = 2.5;' in pointer
assert '[owners setObject:self forKey:mouse]' in registration
removal = body(pointer, '-(void)unregisterMouseCallbacks:')
assert '[owners objectForKey:mouse] != self' in removal
assert 'mouse.mouseInput.mouseMovedHandler = nil;' in removal

display = body(pointer, 'static CVReturn displayLinkOutputCallback')
assert 'mouseDelta' not in display and 'gcMouse' not in display
assert display.count('LiSendMouseMoveEventCtx') == 1
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
             'if (suppressed)'):
    assert relative.index(gate) < relative.index('HIDDispatchInput'), gate
queued = body(relative, 'HIDDispatchInput(self, inputCtx, ^')
for token in ('_relativeMotionQueueEpoch != epoch', '_relativeMotionLastSource != source',
              'memset(_relativeMotionResiduals',
              'self.inputContext != inputCtx', 'epoch != __atomic_load_n',
              'HIDSubmitRelativeMotion(&_relativeMotionResiduals[source]',
              'deltaX * sensitivity, deltaY * sensitivity', 'HIDSendRelativeChunk, &sender',
              'LiSendMouseMoveEventCtx', 'Log(LOG_W', 'Mouse Runtime Path GameController Active',
              'Mouse Runtime Detail GameController Active'):
    assert token in queued, token
assert 'HIDScaledRelativeDelta' not in core + pointer + internal
for text, signatures in ((core, ('- (void)setShouldSendInputEvents:', '- (void)setInputContext:',
                                 '- (void)refreshMouseInputConfiguration', '- (void)tearDownCoreHIDMouseDriver',
                                 'didFailWithReason:')),
                         (pointer, ('- (void)setFreeMouseVirtualCursorActive:',
                                    '-(void)registerMouseCallbacks:', '-(void)unregisterMouseCallbacks:'))):
    for signature in signatures:
        assert 'resetPointerInputLifecycle' in body(text, signature), signature
for signature in ('- (void)suppressRelativeMouseMotionForMilliseconds:', '- (void)resetFreeMouseVirtualCursorState'):
    assert 'resetRelativeMouseMotion' in body(pointer, signature), signature
swift = (root / 'Limelight/Input/CoreHIDMouseDriver.swift').read_text()
report = body(swift, 'private func reportDelta(')
assert report.count('didReceiveDeltaX: deltaX, deltaY: deltaY') == 1
assert 'didObserveRawDeltaX' in report and 'markMovementEventDelivered()' in report
for obsolete in ('pendingDelta', 'flushTask', 'Task.sleep', 'maximumReportRate', 'lastDispatchTimestamp'):
    assert obsolete not in swift, obsolete
absolute = body(pointer, '- (void)sendCoalescedAbsoluteMousePositionForViewPoint:')
assert 'pendingCoalescedAbsolutePointer' not in pointer + internal
assert 'dispatchPendingCoalescedAbsolutePointerPosition' not in pointer
assert 'lastAbsolutePointerHostX ==' not in pointer
assert 'hostX == self.lastAbsolutePointerHostX' not in pointer
assert 'HIDDispatchInput(self, inputCtx, ^{' in absolute
assert 'LiSendMousePositionEventCtx(inputCtx, hostX, hostY, referenceWidth, referenceHeight)' in absolute
assert '[sourceTag copy]' in absolute and 'self.inputContext != inputCtx' in absolute
print('PASS: static immediate GC/CoreHID, per-source queued conversion/reset wiring, absolute snapshots and controller-only display lifecycle')

for button in ('leftButton', 'middleButton', 'rightButton', 'auxiliaryButtons[0]', 'auxiliaryButtons[1]'):
    handler = body(registration, f'mouse.mouseInput.{button}.pressedChangedHandler = ^')
    queued = body(handler, 'HIDDispatchInput(self, inputCtx, ^')
    assert handler.count('HIDDispatchInput(') == 1
    assert handler.count('LiSendMouseButtonEventCtx(') == queued.count('LiSendMouseButtonEventCtx(') == 1
    assert 'Log(LOG_W' in body(queued, 'if (err != 0)')
assert 'handleGCMouseScrollValueY' not in scroll + internal + pointer
for callbacks in (registration, removal):
    for component in ('scroll', 'scroll.xAxis', 'scroll.yAxis'):
        assert f'mouse.mouseInput.{component}.valueChangedHandler = nil;' in callbacks
    assert 'scroll.yAxis.valueChangedHandler = ^' not in callbacks
method = body(scroll, '- (void)scrollWheel:')
for token in ('shouldSuppressScrollEvent:event', '!self.shouldSendInputEvents',
              'HIDValidateInputContext', 'event.deltaX', 'event.deltaY',
              'event.hasPreciseScrollingDeltas', 'event.phase', 'event.momentumPhase',
              'reverseScrollDirectionFor:', '[self inputWheelEpoch]'):
    assert method.index(token) < method.index('HIDDispatchInput'), token
queued = body(method, 'HIDDispatchInput(self, inputCtx, ^')
assert not re.search(r'\bevent\b', queued)
for token in ('epoch != [self inputWheelEpoch]', '!self.shouldSendInputEvents',
              'self.inputContext != inputCtx', 'self.wheelQueueEpoch == epoch',
              '(HIDWheelState){0}', 'HIDWheelFromCocoa(&state, deltaX, deltaY, precise',
              'self.wheelState = state', 'self.wheelQueueEpoch = epoch',
              'HIDWheelSendEvent(wheel, reverse, reverse, HIDSubmitWheelPacket',
              'Log(LOG_W', 'err ? 0 : (short)', 'if (!err) LiNoteScrollTraceLocalDispatchCtx'):
    assert token in queued, token
for name in ('LiSendScrollEventCtx', 'LiSendHScrollEventCtx',
             'LiSendHighResScrollEventCtx', 'LiSendHighResHScrollEventCtx'):
    assert queued.count(name + '(') == method.count(name + '(') == 1
    assert queued.index(name) < queued.index('if (!err) LiNoteScrollTraceLocalDispatchCtx')
for obsolete in ('scrollingDelta', 'CGEvent', 'isDirectionInvertedFromDevice', 'useGCMouse',
                 'suppressAppKitScroll', 'HIDDeduplicatedScrollClick', 'HIDClassifyAppKitScrollEvent'):
    assert obsolete not in scroll, obsolete
assert 'return __atomic_load_n(&_relativeMotionEpoch, __ATOMIC_ACQUIRE);' in body(core, '- (uint64_t)inputMotionEpoch')
ui = (root / 'Limelight/macOS/ViewControllers/SettingsInputPane.swift').read_text()
for obsolete in ('wheelScrollSpeed', 'physicalWheelHighPrecisionScale', 'selectedPhysicalWheelMode',
                 'selectedRewrittenScrollMode', 'rewrittenScrollSpeed', 'smartWheelTailFilter',
                 'gestureScrollSpeed', 'coreHIDMaxMouseReportRate'):
    assert '$settingsModel.' + obsolete not in ui, obsolete
assert '$settingsModel.reverseScrollDirection' in ui
print('PASS: static five GC button wrappers; AppKit-only two-axis wheel snapshots, wheel epoch, success-only tracing and inactive tuning UI (not runtime)')

for signature in ('- (void)keyDown:', '- (void)keyUp:', '- (void)releaseAllModifierKeys {'):
    method = body(core, signature)
    before, queued = method.split('HIDDispatchInput(self, inputCtx, ^{', 1)
    assert '_pressedKeyPairs' not in before
    assert '[_pressedKeyPairsLock lock]' in queued
    assert 'LiSendKeyboardEvent2Ctx' in queued and 'if (err == 0)' in queued
down = body(core, '- (void)keyDown:')
assert down.index('if (event.isARepeat) return;') < down.index('self.shouldSendInputEvents')
assert down.index('if (event.isARepeat) return;') < down.index('syncKeyboardModifierStateForEvent')
print('PASS: static tracked keyboard state remains queued and success-guarded (not runtime)')
