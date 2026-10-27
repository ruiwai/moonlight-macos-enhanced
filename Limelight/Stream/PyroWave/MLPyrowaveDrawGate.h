// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_DRAW_GATE_H
#define ML_PYROWAVE_DRAW_GATE_H

#include <stdbool.h>

// Zero-initialized; caller serializes every access and owns stop/epoch checks.
// Notification/admission state only: never owns a frame, UI object or callback.
typedef struct {
    bool pending, queued, inFlight, attempted;
} MLPyrowaveDrawGate;

static inline bool MLPyrowaveDrawGateRequest(MLPyrowaveDrawGate *g)
{
    if (!g->pending || g->queued || g->inFlight) return false;
    g->queued = true;
    g->attempted = false;
    return true;
}

static inline bool MLPyrowaveDrawGateBeginSubmission(MLPyrowaveDrawGate *g)
{
    if (!g->pending || g->inFlight) return false;
    g->pending = false;
    g->inFlight = true;
    g->attempted = true;
    return true;
}

static inline void MLPyrowaveDrawGateEndSubmission(MLPyrowaveDrawGate *g,
                                                 bool submitted, bool retryBusy)
{
    if (!submitted) {
        g->inFlight = false;
        if (retryBusy) {
            g->pending = true;
            g->attempted = false; // Busy waits for another producer/resume, no spin.
        }
    }
}

static inline void MLPyrowaveDrawGateComplete(MLPyrowaveDrawGate *g)
{
    g->inFlight = false;
}

static inline bool MLPyrowaveDrawGateFinishNotification(MLPyrowaveDrawGate *g)
{
    g->queued = false;
    const bool request = g->attempted && g->pending && !g->inFlight;
    g->attempted = false;
    return request;
}

static inline void MLPyrowaveDrawGateReset(MLPyrowaveDrawGate *g)
{
    const MLPyrowaveDrawGate zero = {false, false, false, false};
    *g = zero;
}
#endif
