#include "HIDRelativeMotionPolicy.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>

typedef struct {
    int calls, failAt;
    int64_t x, y;
    int16_t firstX, firstY, lastX, lastY;
} Sink;

static int send(void *opaque, int16_t x, int16_t y) {
    Sink *s = opaque;
    assert(x || y);
    if (++s->calls == 1) { s->firstX = x; s->firstY = y; }
    s->lastX = x; s->lastY = y;
    if (s->calls == s->failAt) return -17;
    s->x += x; s->y += y;
    return 0;
}

int main(void) {
    HIDRelativeMotionResidual r = {0};
    Sink s = {0};
    assert(!HIDSubmitRelativeMotion(&r, 0, 0, send, &s) && !s.calls);
    for (int i = 0; i < 3; i++)
        assert(!HIDSubmitRelativeMotion(&r, .25, -.25, send, &s) && !s.calls);
    assert(r.x == .75 && r.y == -.75);
    assert(!HIDSubmitRelativeMotion(&r, .25, -.25, send, &s));
    assert(s.calls == 1 && s.x == 1 && s.y == -1 && r.x == 0 && r.y == 0);
    assert(!HIDSubmitRelativeMotion(&r, -.75, .75, send, &s));
    assert(!HIDSubmitRelativeMotion(&r, .5, -.5, send, &s) && s.calls == 1);
    assert(r.x == -.25 && r.y == .25); // Truncate toward zero, not floor/min-one.
    assert(!HIDSubmitRelativeMotion(&r, -1, 1, send, &s));
    assert(s.calls == 2 && s.lastX == -1 && s.lastY == 1);
    assert(!HIDSubmitRelativeMotion(&r, 2.5, -2.5, send, &s));
    assert(s.lastX == 2 && s.lastY == -2 && r.x == .25 && r.y == -.25);
    puts("PASS: zero, fractional accumulation, cancellation and reversal");

    r = (HIDRelativeMotionResidual){0}; s = (Sink){0};
    assert(!HIDSubmitRelativeMotion(&r, 100000.5, -100000.5, send, &s));
    assert(s.calls == 4 && s.x == 100000 && s.y == -100000);
    assert(s.firstX == 32767 && s.firstY == -32768 && s.lastX == 1699 && s.lastY == -1696);
    assert(r.x == .5 && r.y == -.5);
    r = (HIDRelativeMotionResidual){0}; s = (Sink){0};
    assert(!HIDSubmitRelativeMotion(&r, -100000, 100000, send, &s));
    assert(s.calls == 4 && s.x == -100000 && s.y == 100000);
    double max = (double)INT16_MAX * HIDRelativeMotionMaxChunks;
    double min = (double)INT16_MIN * HIDRelativeMotionMaxChunks;
    s = (Sink){0};
    assert(!HIDSubmitRelativeMotion(&r, max, min, send, &s));
    assert(s.calls == 4096 && s.x == (int64_t)max && s.y == (int64_t)min);
    puts("PASS: both large signs and exact 4096-chunk limits without clamp loss");

    double invalid[] = {NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX, max + 2, min - 2};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (int axis = 0; axis < 2; axis++) {
            r = (HIDRelativeMotionResidual){.25, -.25}; s = (Sink){0};
            assert(HIDSubmitRelativeMotion(&r, axis ? invalid[i] : 1,
                       axis ? 1 : invalid[i], send, &s) == HIDRelativeMotionInvalid);
            assert(!s.calls && r.x == 0 && r.y == 0);
        }
    }
    r = (HIDRelativeMotionResidual){0}; s = (Sink){0};
    assert(HIDSubmitRelativeMotion(&r, max + 1, 1, send, &s) == HIDRelativeMotionInvalid && !s.calls);
    assert(HIDSubmitRelativeMotion(&r, 1, min - 1, send, &s) == HIDRelativeMotionInvalid && !s.calls);
    puts("PASS: either malformed axis rejects the entire submission before any send");

    for (int fail = 1; fail <= 4; fail++) {
        r = (HIDRelativeMotionResidual){0}; s = (Sink){.failAt = fail};
        assert(HIDSubmitRelativeMotion(&r, 100000.25, -100000.25, send, &s) == -17);
        assert(s.calls == fail && r.x == .25 && r.y == -.25);
        int calls = s.calls;
        assert(!HIDSubmitRelativeMotion(&r, 0, 0, send, &s) && s.calls == calls);
        assert(!HIDSubmitRelativeMotion(&r, .75, -.75, send, &s));
        assert(s.calls == calls + 1 && s.lastX == 1 && s.lastY == -1);
    }
    puts("PASS: first enqueue failure stops; dropped integers are never retried");

    HIDRelativeMotionResidual sources[3] = {{0}};
    s = (Sink){0};
    for (int i = 0; i < 3; i++)
        assert(!HIDSubmitRelativeMotion(&sources[i], .75, 0, send, &s) && !s.calls);
    sources[0] = (HIDRelativeMotionResidual){0}; // Lifecycle reset drops fractions.
    assert(!HIDSubmitRelativeMotion(&sources[0], .25, 0, send, &s) && !s.calls);
    assert(!HIDSubmitRelativeMotion(&sources[1], .25, 0, send, &s) && s.calls == 1);
    puts("PASS: independent source residuals and explicit reset");
}
