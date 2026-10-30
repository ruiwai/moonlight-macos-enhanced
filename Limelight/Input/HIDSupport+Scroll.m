// AppKit is the sole wheel source, including when GC supplies motion/buttons.
#import "HIDSupport_Internal.h"

typedef int (^HIDWheelSubmitter)(bool, bool, int);
static int HIDSubmitWheelPacket(void *opaque, bool horizontal, bool smooth, int amount) {
    return (*(HIDWheelSubmitter __strong *)opaque)(horizontal, smooth, amount);
}

@implementation HIDSupport (Scroll)

- (void)scrollWheel:(NSEvent *)event {
    if ([self.precisionTouchpad shouldSuppressScrollEvent:event] || !self.shouldSendInputEvents) {
        [self resetMouseWheel];
        return;
    }
    PML_INPUT_STREAM_CONTEXT inputCtx = HIDInputContext(self);
    if (!HIDValidateInputContext(inputCtx, "scrollWheel")) return;

    // Capture all event/config values before dispatch; never retain NSEvent on inputQueue.
    const double deltaX = event.deltaX, deltaY = event.deltaY;
    const BOOL precise = event.hasPreciseScrollingDeltas;
    const NSEventPhase phase = event.phase, momentum = event.momentumPhase;
    const BOOL reverse = [SettingsClass reverseScrollDirectionFor:self.host.uuid];
    const uint64_t epoch = [self inputWheelEpoch];
    const uint64_t traceId = [self prepareScrollTraceFromSource:@"appkit"
        rawDeltaX:deltaX rawDeltaY:deltaY phase:phase momentumPhase:momentum hasPreciseDeltas:precise];

    HIDDispatchInput(self, inputCtx, ^{
        if (epoch != [self inputWheelEpoch] || !self.shouldSendInputEvents || self.inputContext != inputCtx) return;
        HIDWheelState state = self.wheelQueueEpoch == epoch ? self.wheelState : (HIDWheelState){0};
        unsigned rejected = 0;
        HIDWheelEvent wheel = HIDWheelFromCocoa(&state, deltaX, deltaY, precise, &rejected);
        self.wheelState = state;
        self.wheelQueueEpoch = epoch;
        __block BOOL recorded = NO;
        HIDWheelSubmitter submit = ^int(bool horizontal, bool smooth, int amount) {
            uint64_t dispatchTime = LiGetMillis();
            int err;
            if (smooth) {
                err = horizontal ? LiSendHighResHScrollEventCtx(inputCtx, (short)amount)
                                 : LiSendHighResScrollEventCtx(inputCtx, (short)amount);
            } else {
                err = horizontal ? LiSendHScrollEventCtx(inputCtx, (signed char)amount)
                                 : LiSendScrollEventCtx(inputCtx, (signed char)amount);
            }
            // Discrete API units are clicks; trace/diagnostic units are 1/120-click.
            short submitted = err ? 0 : (short)(smooth ? amount : amount * 120);
            if (!err) LiNoteScrollTraceLocalDispatchCtx(inputCtx, traceId, dispatchTime,
                                                       submitted, smooth, horizontal);
            [self recordScrollInputDiagnosticsMode:err ? @"appkit-qt-enqueue-failed" :
                (smooth ? @"appkit-qt-smooth" : @"appkit-qt-click")
                traceId:traceId rawDeltaX:deltaX rawDeltaY:deltaY rawWheelDeltaX:0 rawWheelDeltaY:0
                normalizedDeltaX:wheel.x.precise normalizedDeltaY:wheel.y.precise
                continuous:smooth hasPreciseDeltas:precise lineDeltaX:wheel.x.integral lineDeltaY:wheel.y.integral
                pointDeltaX:0 pointDeltaY:0 fixedDeltaXRaw:0 fixedDeltaYRaw:0 phase:phase momentumPhase:momentum
                dispatchedX:horizontal ? submitted : 0 dispatchedY:horizontal ? 0 : submitted];
            recorded = YES;
            return err;
        };
        int err = HIDWheelSendEvent(wheel, reverse, reverse, HIDSubmitWheelPacket, &submit, &rejected);
        if (err) Log(LOG_W, @"[appkit] wheel enqueue failed: %d (remaining axes/clicks stopped)", err);
        if (rejected) Log(LOG_W, @"[appkit] wheel rejected nonfinite/out-of-range axes: %u", rejected);
        if (!recorded) {
            [self recordScrollInputDiagnosticsMode:rejected ? @"appkit-qt-rejected" : @"appkit-qt-no-packet"
                traceId:traceId rawDeltaX:deltaX rawDeltaY:deltaY rawWheelDeltaX:0 rawWheelDeltaY:0
                normalizedDeltaX:wheel.x.precise normalizedDeltaY:wheel.y.precise
                continuous:NO hasPreciseDeltas:precise lineDeltaX:wheel.x.integral lineDeltaY:wheel.y.integral
                pointDeltaX:0 pointDeltaY:0 fixedDeltaXRaw:0 fixedDeltaYRaw:0 phase:phase momentumPhase:momentum
                dispatchedX:0 dispatchedY:0];
        }
        [SettingsClass updateScrollInputRuntimeStatusFor:self.host.uuid
            summaryKey:@"AppKit wheel (SDL2/Qt)"
            detailKey:err ? @"Enqueue failed; remaining wheel sends stopped." :
                (rejected ? @"Rejected nonfinite or out-of-range wheel axes." :
                 (recorded ? @"Wheel packets enqueued (not a host acknowledgement)." : @"No nonzero wheel packet."))];
    });
}
@end
