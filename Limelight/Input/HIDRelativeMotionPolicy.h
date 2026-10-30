#ifndef HID_RELATIVE_MOTION_POLICY_H
#define HID_RELATIVE_MOTION_POLICY_H

#include <math.h>
#include <stdint.h>

enum { HIDRelativeMotionMaxChunks = 4096, HIDRelativeMotionInvalid = -2147483647 };
typedef struct { double x, y; } HIDRelativeMotionResidual;
typedef int (*HIDRelativeMotionSender)(void *, int16_t, int16_t);

// Serial-owner only. Validate both axes before sending anything. Integer motion
// is consumed even on enqueue failure; only the fractional tail survives.
static inline int HIDSubmitRelativeMotion(HIDRelativeMotionResidual *residual,
                                          double dx, double dy,
                                          HIDRelativeMotionSender send, void *opaque) {
    double x = residual->x + dx, y = residual->y + dy;
    double ix = trunc(x), iy = trunc(y);
    if (!isfinite(x) || !isfinite(y) ||
        ix > (double)INT16_MAX * HIDRelativeMotionMaxChunks ||
        iy > (double)INT16_MAX * HIDRelativeMotionMaxChunks ||
        ix < (double)INT16_MIN * HIDRelativeMotionMaxChunks ||
        iy < (double)INT16_MIN * HIDRelativeMotionMaxChunks) {
        *residual = (HIDRelativeMotionResidual){0};
        return HIDRelativeMotionInvalid;
    }
    residual->x = x - ix;
    residual->y = y - iy;
    int64_t remainingX = (int64_t)ix, remainingY = (int64_t)iy;
    while (remainingX || remainingY) {
        int16_t chunkX = remainingX > INT16_MAX ? INT16_MAX :
                         remainingX < INT16_MIN ? INT16_MIN : (int16_t)remainingX;
        int16_t chunkY = remainingY > INT16_MAX ? INT16_MAX :
                         remainingY < INT16_MIN ? INT16_MIN : (int16_t)remainingY;
        int err = send(opaque, chunkX, chunkY);
        if (err != 0) return err;
        remainingX -= chunkX;
        remainingY -= chunkY;
    }
    return 0;
}

#endif
