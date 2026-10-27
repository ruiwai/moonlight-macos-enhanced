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
    MLPyrowaveDrawGate g = {false, false, false, false, false};
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

static void completionBeforeEnd(void)
{
    MLPyrowaveDrawGate g = {false, false, false, false, false};
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateComplete(&g); // Callback may beat backend's synchronous return.
    assert(!g.inFlight && g.queued && g.attempted);
    MLPyrowaveDrawGateEndSubmission(&g, true, false);
    assert(!g.inFlight); // End must never re-arm a completed submission.
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(!MLPyrowaveDrawGateRequest(&g));

    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    for (unsigned i = 0; i < 100; ++i) assert(!offer(&g));
    MLPyrowaveDrawGateComplete(&g);
    assert(!MLPyrowaveDrawGateRequest(&g)); // Existing notification still owns draw.
    for (unsigned i = 0; i < 100; ++i) assert(!offer(&g));
    MLPyrowaveDrawGateEndSubmission(&g, true, false);
    assert(g.pending && g.queued && !g.inFlight && g.attempted);
    assert(MLPyrowaveDrawGateFinishNotification(&g));
    assert(MLPyrowaveDrawGateRequest(&g)); // Exactly one follow-up notification.
    assert(!MLPyrowaveDrawGateRequest(&g));
}

static void noSubmissionOrBusy(void)
{
    MLPyrowaveDrawGate g = {false, false, false, false, false};
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

// All receive completions below are IGNORED: no offer/pending producer update.
static void receiveContention(void)
{
    for (unsigned order = 0; order < 3; ++order) {
        MLPyrowaveDrawGate g = {false, false, false, false, false};
        assert(!MLPyrowaveDrawGateReceiveComplete(&g));
        assert(!g.attempted);
        assert(offer(&g));
        assert(MLPyrowaveDrawGateBeginSubmission(&g));
        // 0: receive completes before render reports contention.
        if (order == 0) assert(!MLPyrowaveDrawGateReceiveComplete(&g));
        MLPyrowaveDrawGateWaitForReceive(&g, order == 0);
        assert(g.pending && !g.inFlight && g.queued);
        assert(g.waitingForReceive == (order != 0));
        if (order != 0) {
            assert(!g.attempted);
            assert(!MLPyrowaveDrawGateRequest(&g));
            assert(!MLPyrowaveDrawGateBeginSubmission(&g)); // Waiting rejects a new Begin.
            if (order == 2) { // Receive completes after notification end.
                assert(!MLPyrowaveDrawGateFinishNotification(&g));
                for (unsigned i = 0; i < 100; ++i) {
                    assert(!MLPyrowaveDrawGateRequest(&g)); // No waiting self-spin.
                    assert(!MLPyrowaveDrawGateBeginSubmission(&g));
                }
            }
            // 1: receive completes after Wait but before Finish.
            assert(MLPyrowaveDrawGateReceiveComplete(&g));
        }
        assert(!g.waitingForReceive && g.attempted && g.pending);
        assert(!MLPyrowaveDrawGateReceiveComplete(&g)); // No duplicate wake/reset of attempted.
        assert(g.attempted);
        if (order != 2) {
            assert(!MLPyrowaveDrawGateRequest(&g)); // Existing notification owns retry.
            assert(MLPyrowaveDrawGateFinishNotification(&g));
        }
        assert(MLPyrowaveDrawGateRequest(&g)); // Exactly one retry, even for IGNORED.
        assert(!MLPyrowaveDrawGateRequest(&g));
        assert(MLPyrowaveDrawGateBeginSubmission(&g));
        MLPyrowaveDrawGateEndSubmission(&g, false, false); // Ordinary empty/Dropped never spins.
        assert(!MLPyrowaveDrawGateFinishNotification(&g));
        assert(!MLPyrowaveDrawGateRequest(&g));
    }
}

static void reset(void)
{
    MLPyrowaveDrawGate g = {true, true, true, true, true};
    MLPyrowaveDrawGateReset(&g);
    assert(!g.pending && !g.queued && !g.inFlight && !g.attempted && !g.waitingForReceive);
    MLPyrowaveDrawGateReset(&g);
    assert(!MLPyrowaveDrawGateRequest(&g));
    assert(!MLPyrowaveDrawGateFinishNotification(&g));
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    MLPyrowaveDrawGateReset(&g);
    assert(!g.pending && !g.queued && !g.inFlight && !g.attempted && !g.waitingForReceive);
    // Reset has no generation tag. App must reject old End/Complete/Finish calls
    // under its lock using epoch/peer identity, even if the new gate is busy.
    assert(offer(&g));
    assert(MLPyrowaveDrawGateBeginSubmission(&g));
    assert(g.queued && g.inFlight && g.attempted && !g.pending);
}

int main(void)
{
    latestWhileBusy();
    completionBeforeEnd();
    noSubmissionOrBusy();
    receiveContention();
    reset();
    puts("PyroWave draw gate CPU state checks passed (not AppKit/GPU validation)");
    return 0;
}
