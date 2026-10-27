// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_STATS_H
#define ML_PYROWAVE_STATS_H
#include <stdbool.h>
#include <stdint.h>

enum { MLPyrowaveStatsValidated, MLPyrowaveStatsDecoded, MLPyrowaveStatsPresented,
       MLPyrowaveStatsPacket };
typedef struct {
    uint64_t startMs, lastMs, bytes, decodeMs;
    uint32_t frames[3];
} MLPyrowaveStatsWindow;
typedef struct {
    MLPyrowaveStatsWindow window;
    float rates[3];
    bool started, hasRates;
    uint64_t lastDecodedMs, lastPresentedMs;
} MLPyrowaveStats;

static inline uint64_t MLPyrowaveStatsDuration(uint64_t start, uint64_t end) {
    return end > start ? end - start : 0;
}
static inline bool MLPyrowaveStatsRecent(uint64_t progress, uint64_t now) {
    return progress != 0 && now >= progress && now - progress <= 1500;
}
static inline float MLPyrowaveStatsRate(const MLPyrowaveStats *s, unsigned index) {
    uint64_t duration = MLPyrowaveStatsDuration(s->window.startMs, s->window.lastMs);
    return s->hasRates ? s->rates[index] :
        (duration ? (float)((double)s->window.frames[index] * 1000.0 / duration) : 0);
}
// Caller owns synchronization. Only real events advance freshness. A completed
// window includes its closing event; the next event opens a new window at that
// boundary. Rates survive short windows; counts/bytes always describe window.
static inline void MLPyrowaveStatsEvent(MLPyrowaveStats *s, uint64_t now,
                                      unsigned event, uint64_t bytes, uint64_t decodeMs) {
    MLPyrowaveStatsWindow *w = &s->window;
    uint64_t eventMs = now;
    if (!s->started) { w->startMs = w->lastMs = now; s->started = true; }
    if (now < w->lastMs) now = w->lastMs;
    if (MLPyrowaveStatsDuration(w->startMs, w->lastMs) >= 1000) {
        uint64_t boundary = w->lastMs;
        MLPyrowaveStatsWindow empty = {0, 0, 0, 0, {0, 0, 0}};
        *w = empty;
        w->startMs = boundary;
    }
    w->lastMs = now;
    w->bytes += bytes;
    if (event < 3) w->frames[event]++;
    if (event == MLPyrowaveStatsDecoded) {
        w->decodeMs += decodeMs;
        if (eventMs > s->lastDecodedMs) s->lastDecodedMs = eventMs;
    }
    if (event == MLPyrowaveStatsPresented && eventMs > s->lastPresentedMs) s->lastPresentedMs = eventMs;
    uint64_t duration = MLPyrowaveStatsDuration(w->startMs, w->lastMs);
    if (duration >= 1000) {
        for (unsigned i = 0; i < 3; ++i) s->rates[i] = (float)((double)w->frames[i] * 1000.0 / duration);
        s->hasRates = true;
    }
}
#endif
