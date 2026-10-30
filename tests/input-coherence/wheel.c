#include "HIDMouseWheelPolicy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { bool horizontal, smooth; int amount; } Packet;
typedef struct { Packet packets[8200]; int count, failAt; } Recorder;
static Recorder recorder;
static int sendPacket(void *opaque, bool horizontal, bool smooth, int amount) {
    Recorder *r = opaque;
    assert(r->count < 8200 && amount != 0);
    r->packets[r->count++] = (Packet){horizontal, smooth, amount};
    return r->failAt == r->count ? 37 : 0;
}
static void clear(void) { memset(&recorder, 0, sizeof(recorder)); }
static void packet(int i, bool horizontal, bool smooth, int amount) {
    assert(i < recorder.count);
    Packet p = recorder.packets[i];
    assert(p.horizontal == horizontal && p.smooth == smooth && p.amount == amount);
}
static int submit(HIDWheelEvent event, bool rx, bool ry, unsigned *rejected) {
    return HIDWheelSendEvent(event, rx, ry, sendPacket, &recorder, rejected);
}
static void clicks(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        for (int count = 128; count <= 4096; count *= 2) {
            clear(); unsigned rejected = 0;
            HIDWheelEvent e = {.y = {sign * count, (float)(sign * count)}};
            assert(submit(e, false, false, &rejected) == 0 && !rejected);
            assert(recorder.count == count);
            for (int i = 0; i < count; i++) packet(i, false, false, sign);
        }
        clear(); unsigned rejected = 0;
        HIDWheelEvent e = {.y = {sign * 4097, (float)(sign * 4097)}, .x = {1, 1}};
        assert(submit(e, false, false, &rejected) == 0);
        assert(rejected == HIDWheelRejectedY && recorder.count == 1);
        packet(0, true, false, 1); // Rejection is not an enqueue failure (Qt).
    }
    clear(); unsigned rejected = 0;
    HIDWheelState state = {0};
    for (int i = 0; i < 100; i++) {
        HIDWheelEvent e = HIDWheelFromCocoa(&state, 0, 1, false, &rejected);
        assert(submit(e, false, false, &rejected) == 0);
    }
    assert(!rejected && recorder.count == 100); // No timing/dedup heuristic.
    for (int i = 0; i < 100; i++) packet(i, false, false, 1);
}
static void axesAndReverse(void) {
    for (int rx = 0; rx < 2; rx++) for (int ry = 0; ry < 2; ry++) {
        clear(); unsigned rejected = 0; HIDWheelState state = {0};
        HIDWheelEvent e = HIDWheelFromCocoa(&state, 2, -3, false, &rejected);
        assert(e.x.integral == -2 && e.x.precise == -2);
        assert(e.y.integral == -3 && e.y.precise == -3);
        assert(!submit(e, rx, ry, &rejected) && !rejected && recorder.count == 5);
        for (int i = 0; i < 3; i++) packet(i, false, false, ry ? 1 : -1);
        for (int i = 3; i < 5; i++) packet(i, true, false, rx ? 1 : -1);
        clear(); e = (HIDWheelEvent){.x = {0, .25f}, .y = {0, -.5f}};
        assert(!submit(e, rx, ry, &rejected) && recorder.count == 2);
        packet(0, false, true, ry ? 60 : -60);
        packet(1, true, true, rx ? -30 : 30);
    }
    clear(); unsigned rejected = 0;
    HIDWheelEvent e = {.x = {3, 0}, .y = {-2, 0}}; // Integral fallback.
    assert(!submit(e, false, true, &rejected) && recorder.count == 5);
    packet(0, false, false, 1); packet(2, true, false, 1);
}
static void cocoaAndAccumulation(void) {
    HIDWheelState state = {0}; unsigned rejected = 0;
    HIDWheelEvent e = HIDWheelFromCocoa(&state, .01, -.01, false, &rejected);
    assert(e.x.integral == -1 && e.y.integral == -1);
    assert(e.x.precise == -1 && e.y.precise == -1);
    e = HIDWheelFromCocoa(&state, -1.01, 1.01, false, &rejected);
    assert(e.x.integral == 2 && e.y.integral == 2); // Away from zero on each axis.
    e = HIDWheelFromCocoa(&state, DBL_MIN, -DBL_MIN, false, &rejected);
    assert(e.x.precise == -1 && e.y.precise == -1); // Round CGFloat before float narrowing.
    e = HIDWheelFromCocoa(&state, -nextafter(1., 2.), nextafter(1., 2.), false, &rejected);
    assert(e.x.precise == 2 && e.y.precise == 2);
    state = (HIDWheelState){0}; clear();
    e = HIDWheelFromCocoa(&state, -.75, .75, true, &rejected);
    assert(!e.x.integral && !e.y.integral && state.x == .75f && state.y == .75f);
    e = HIDWheelFromCocoa(&state, -.75, .75, true, &rejected);
    assert(e.x.integral == 1 && e.y.integral == 1 && e.y.precise == .75f);
    assert(state.x == .5f && state.y == .5f);
    assert(!submit(e, false, false, &rejected) && recorder.count == 2);
    packet(0, false, true, 90); packet(1, true, true, 90); // Mismatch is smooth.
    e = HIDWheelFromCocoa(&state, .25, -.25, true, &rejected);
    assert(!e.x.integral && !e.y.integral && state.x == -.25f && state.y == -.25f);
    e = HIDWheelFromCocoa(&state, .75, -.75, true, &rejected);
    assert(e.x.integral == -1 && e.y.integral == -1 && state.y == 0);
    clear(); state = (HIDWheelState){0};
    for (int i = 0; i < 250; i++) {
        e = HIDWheelFromCocoa(&state, 0, .004, true, &rejected);
        assert(!submit(e, false, false, &rejected));
    }
    assert(!recorder.count && !rejected); // No smooth 120-unit residual accumulation.
    state = (HIDWheelState){0}; // Lifecycle's lazy epoch reset uses this same value.
    e = HIDWheelFromCocoa(&state, 0, .5, true, &rejected);
    assert(!e.y.integral && state.y == .5f);
}
static void smoothAndGuards(void) {
    const float amounts[] = {.5f, -.5f, 1.5f, -1.5f, FLT_MAX, -FLT_MAX, .004f, -.004f};
    const int expected[] = {60, -60, 120, -120, 120, -120, 0, 0};
    for (unsigned i = 0; i < sizeof(amounts)/sizeof(*amounts); i++) {
        clear(); unsigned rejected = 0;
        HIDWheelEvent e = {.y = {0, amounts[i]}};
        assert(!submit(e, false, false, &rejected) && !rejected);
        assert(recorder.count == (expected[i] != 0));
        if (expected[i]) packet(0, false, true, expected[i]);
    }
    const double bad[] = {NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX, -2147483904.};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        clear(); unsigned rejected = 0; HIDWheelState state = {.25f, -.25f};
        HIDWheelEvent e = HIDWheelFromCocoa(&state, bad[i], bad[i], true, &rejected);
        assert(rejected == 3 && state.x == 0 && state.y == 0);
        assert(!submit(e, false, false, &rejected) && !recorder.count);
    }
    const float badPrecise[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < 3; i++) {
        clear(); unsigned rejected = 0;
        HIDWheelEvent e = {.x = {1, badPrecise[i]}, .y = {1, badPrecise[i]}};
        assert(!submit(e, false, false, &rejected) && rejected == 3 && !recorder.count);
    }
    clear(); unsigned rejected = 0;
    HIDWheelEvent e = {.x = {INT32_MIN, (float)INT32_MIN}, .y = {INT32_MAX, 0}};
    assert(!submit(e, true, true, &rejected) && rejected == 3 && !recorder.count);
    HIDWheelState state = {0}; rejected = 0;
    e = HIDWheelFromCocoa(&state, 2147483648., 2147483648., true, &rejected);
    assert(rejected == HIDWheelRejectedY && e.x.integral == INT32_MIN);
    assert(!submit(e, false, false, &rejected) && rejected == 3);
    state = (HIDWheelState){0}; rejected = 0;
    e = HIDWheelFromCocoa(&state, 0, -2147483648., true, &rejected);
    assert(!rejected && e.y.integral == INT32_MIN);
    assert(!submit(e, false, false, &rejected) && rejected == HIDWheelRejectedY);
}
static void failures(void) {
    for (int failAt = 1; failAt <= 4; failAt++) {
        clear(); unsigned rejected = 0; recorder.failAt = failAt;
        HIDWheelEvent e = {.x = {2, 2}, .y = {-3, -3}};
        assert(submit(e, false, false, &rejected) == 37);
        assert(!rejected && recorder.count == failAt);
        for (int i = 0; i < failAt && i < 3; i++) packet(i, false, false, -1);
        if (failAt == 4) packet(3, true, false, 1);
    }
    clear(); unsigned rejected = 0; recorder.failAt = 1;
    HIDWheelEvent e = {.x = {3, 3}, .y = {0, .25f}};
    assert(submit(e, false, false, &rejected) == 37 && recorder.count == 1);
    packet(0, false, true, 30);
}
int main(void) {
    clicks(); axesAndReverse(); cocoaAndAccumulation(); smoothAndGuards(); failures();
    puts("PASS: production SDL2 Cocoa accumulation + Qt macOS wheel policy, guards and send failures");
}
