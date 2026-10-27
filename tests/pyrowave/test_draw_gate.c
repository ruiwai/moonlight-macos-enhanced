// SPDX-License-Identifier: GPL-3.0-only
#include "MLPyrowaveDrawGate.h"
#include <assert.h>
#include <stdio.h>

static bool offer(MLPyrowaveDrawGate *g)
{
    g->pending = true; // Same offer sequence as the app's validated packet path.
    return MLPyrowaveDrawGateRequest(g);
}

static void latestWhileBusy(void)
{
    MLPyrowaveDrawGate g = {false, false, false, false};
    assert(!MLPyrowaveDrawGateRequest(&g));
    assert(!MLPyrowaveDrawGateBeginSubmission(&g));
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, true, false);
    for (unsigned i = 0; i < 100; ++i) assert(!offer(&g));
    assert(g.pending && g.queued && g.inFlight && g.attempted);
    assert(!MLPyrowaveDrawGateBeginSubmission(&g)); // No extra GPU admission.
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(!MLPyrowaveDrawGateRequest(&g)); // Still busy after draw returns.
    MLPyrowaveDrawGateComplete(&g);
    assert(MLPyrowaveDrawGateRequest(&g)); // Completion wakes exactly one draw.
    assert(!MLPyrowaveDrawGateRequest(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, true, false);
    // Also cover completion before the notification finishes.
    assert(!offer(&g));
    MLPyrowaveDrawGateComplete(&g);
    assert(!MLPyrowaveDrawGateRequest(&g)); // queued spans the entire draw call.
    assert(MLPyrowaveDrawGateFinishNotification(&g));
    assert(MLPyrowaveDrawGateRequest(&g));
    assert(!MLPyrowaveDrawGateRequest(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, true, false);
    MLPyrowaveDrawGateComplete(&g);
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(!MLPyrowaveDrawGateRequest(&g));
}

static void noSubmissionOrBusy(void)
{
    MLPyrowaveDrawGate g = {false, false, false, false};
    assert(offer(&g));
    for (unsigned i = 0; i < 100; ++i) assert(!offer(&g)); // Coalesce queued work.
    // Nil drawable/suspended/invalid draw: no BeginSubmission, no self-spin.
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(g.pending && !g.queued && !g.inFlight && !g.attempted);
    assert(offer(&g)); // Next packet or explicit resume can request again.
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, false, true);
    assert(g.pending && g.queued && !g.inFlight && !g.attempted);
    assert(!MLPyrowaveDrawGateFinishNotification(&g)); // Busy restores, not spins.
    assert(g.pending && !g.queued);
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, false, false); // Dropped: no GPU callback.
    assert(!g.pending && !g.inFlight && g.attempted);
    // A producer after Dropped but before notification finish must be rescheduled.
    assert(!offer(&g));
    assert(MLPyrowaveDrawGateFinishNotification(&g));
    assert(MLPyrowaveDrawGateRequest(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateEndSubmission(&g, false, false);
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(offer(&g)); // Producer after notification finish schedules normally.
}

static void reset(void)
{
    MLPyrowaveDrawGate g = {true, true, true, true};
    MLPyrowaveDrawGateReset(&g);
    assert(!g.pending && !g.queued && !g.inFlight && !g.attempted);
    MLPyrowaveDrawGateReset(&g);
    assert(!MLPyrowaveDrawGateRequest(&g));
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateReset(&g);
    assert(!g.pending && !g.queued && !g.inFlight && !g.attempted);
    // Stop/epoch rejection of stale notifications/completions belongs to owner.
}

int main(void)
{
    latestWhileBusy();
    noSubmissionOrBusy();
    reset();
    puts("PyroWave draw gate CPU state checks passed (not AppKit/GPU validation)");
    return 0;
}
