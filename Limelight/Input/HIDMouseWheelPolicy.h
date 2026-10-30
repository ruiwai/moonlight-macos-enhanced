// SDL2 release-2.32.8 Cocoa/SDL mouse accumulation + Qt mousewheel.cpp (macOS).
// Portable production policy. Caller owns state on its serial input queue.
#ifndef HID_MOUSE_WHEEL_POLICY_H
#define HID_MOUSE_WHEEL_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#include <float.h>
#include <math.h>

typedef struct { float x, y; } HIDWheelState;
typedef struct { int32_t integral; float precise; } HIDWheelAxis;
typedef struct { HIDWheelAxis x, y; } HIDWheelEvent;
// amount is ±1 for a discrete click, otherwise a nonzero signed-16 smooth amount.
typedef int (*HIDWheelSender)(void *, bool horizontal, bool smooth, int amount);
enum { HIDWheelRejectedX = 1, HIDWheelRejectedY = 2, HIDWheelMaxClicks = 4096 };

static inline HIDWheelAxis HIDWheelAccumulate(float *residual, double amount,
                                             bool precise, unsigned *rejected,
                                             unsigned axisBit) {
    HIDWheelAxis axis = {0};
    if (!isfinite(amount) || fabs(amount) > FLT_MAX) goto reject;
    // Cocoa uses deltaX/deltaY, not scrollingDelta*, and no /10 scaling.
    // Round CGFloat first, before SDL_SendMouseWheel narrows to float.
    if (!precise) amount = amount > 0 ? ceil(amount) : floor(amount);
    float delta = (float)amount;
    if ((*residual > 0 && delta < 0) || (*residual < 0 && delta > 0))
        *residual = 0;
    float accumulated = *residual + delta;
    // SDL's integral field is signed-32; guard before float-to-int conversion.
    if (!isfinite(accumulated) || (double)accumulated < INT32_MIN ||
        (double)accumulated > INT32_MAX) goto reject;
    axis.integral = (int32_t)truncf(accumulated);
    axis.precise = delta;
    *residual = accumulated - (float)axis.integral;
    return axis;
reject:
    *residual = 0;
    *rejected |= axisBit;
    return axis;
}

static inline HIDWheelEvent HIDWheelFromCocoa(HIDWheelState *state,
                                             double deltaX, double deltaY,
                                             bool precise, unsigned *rejected) {
    HIDWheelEvent event;
    event.x = HIDWheelAccumulate(&state->x, -deltaX, precise, rejected, HIDWheelRejectedX);
    event.y = HIDWheelAccumulate(&state->y, deltaY, precise, rejected, HIDWheelRejectedY);
    return event;
}

static inline int HIDWheelSendAxis(HIDWheelAxis axis, bool horizontal, bool reverse,
                                   HIDWheelSender send, void *opaque, unsigned *rejected) {
    bool discrete = axis.integral != 0 && axis.precise == (float)axis.integral;
    if (!discrete && axis.precise != 0) {
        if (!isfinite(axis.precise)) goto reject;
        double amount = reverse ? -(double)axis.precise : axis.precise;
        // Qt macOS clamp is per event, not a smoothing accumulator.
        amount = fmax(-1.0, fmin(1.0, amount));
        int scaled = (int)trunc(amount * 120);
        return scaled ? send(opaque, horizontal, true, scaled) : 0;
    }
    // Qt's integral fallback also covers precise == 0.
    int64_t clicks = axis.integral;
    if (reverse) clicks = -clicks;
    int64_t count = clicks < 0 ? -clicks : clicks;
    if (count > HIDWheelMaxClicks) goto reject;
    for (int64_t i = 0; i < count; i++) {
        int result = send(opaque, horizontal, false, clicks < 0 ? -1 : 1);
        if (result != 0) return result;
    }
    return 0;
reject:
    *rejected |= horizontal ? HIDWheelRejectedX : HIDWheelRejectedY;
    return 0;
}

static inline int HIDWheelSendEvent(HIDWheelEvent event, bool reverseX, bool reverseY,
                                    HIDWheelSender send, void *opaque, unsigned *rejected) {
    int result = HIDWheelSendAxis(event.y, false, reverseY, send, opaque, rejected);
    return result ? result : HIDWheelSendAxis(event.x, true, reverseX, send, opaque, rejected);
}
#endif
