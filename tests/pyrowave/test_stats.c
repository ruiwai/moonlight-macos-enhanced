#include "MLPyrowaveStats.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    MLPyrowaveStats s;
    memset(&s, 0, sizeof(s));
    assert(MLPyrowaveStatsRate(&s, 0) == 0);
    assert(MLPyrowaveStatsDuration(9, 8) == 0);
    assert(!MLPyrowaveStatsRecent(0, 10));
    MLPyrowaveStatsEvent(&s, 0, MLPyrowaveStatsPacket, 100, 0);
    assert(s.started && MLPyrowaveStatsRate(&s, 0) == 0);
    MLPyrowaveStatsEvent(&s, 100, MLPyrowaveStatsValidated, 200, 0);
    MLPyrowaveStatsEvent(&s, 120, MLPyrowaveStatsDecoded, 0, 20);
    assert(s.window.frames[0] == 1 && s.window.frames[1] == 1 && s.window.frames[2] == 0);
    assert(s.window.bytes == 300 && s.window.decodeMs == 20);
    MLPyrowaveStatsEvent(&s, 140, MLPyrowaveStatsPresented, 0, 0);
    assert(s.window.frames[1] == 1 && s.window.frames[2] == 1); // No decode double count.
    MLPyrowaveStatsEvent(&s, 2000, MLPyrowaveStatsValidated, 700, 0);
    assert(s.hasRates && fabsf(s.rates[0] - 1.0f) < 0.0001f); // Actual 2 s, not 1 s.
    assert(s.window.bytes == 1000 && s.window.lastMs == 2000);
    MLPyrowaveStatsEvent(&s, 2001, MLPyrowaveStatsPacket, 50, 0);
    assert(s.window.startMs == 2000 && s.window.frames[1] == 0);
    assert(s.window.bytes == 50 && MLPyrowaveStatsRate(&s, 0) == 1.0f);
    assert(MLPyrowaveStatsRate(&s, 1) == 0.5f); // Retain completed rate in short window.
    assert(!MLPyrowaveStatsRecent(s.lastDecodedMs, 2001)); // Packets don't fake GPU progress.
    MLPyrowaveStatsEvent(&s, 2010, MLPyrowaveStatsDecoded, 0, 9);
    MLPyrowaveStatsEvent(&s, 2020, MLPyrowaveStatsPresented, 0, 0);
    assert(MLPyrowaveStatsRecent(s.lastDecodedMs, 2021));
    assert(MLPyrowaveStatsRecent(s.lastPresentedMs, 2021));
    uint64_t last = s.window.lastMs;
    (void)MLPyrowaveStatsRate(&s, 2);
    assert(s.window.lastMs == last && !MLPyrowaveStatsRecent(last, 4000)); // Stalled getter.
    assert(!MLPyrowaveStatsRecent(last, 1000)); // Future/clock guard.
    MLPyrowaveStatsEvent(&s, 1000, MLPyrowaveStatsDecoded, 0, MLPyrowaveStatsDuration(2000, 1000));
    assert(s.window.lastMs == last && s.lastDecodedMs == 2010 && s.window.decodeMs == 9);
    MLPyrowaveStatsEvent(&s, 5000, MLPyrowaveStatsPacket, 100, 0);
    assert(s.window.lastMs == 5000 && s.window.frames[1] == 2);
    MLPyrowaveStatsEvent(&s, 5001, MLPyrowaveStatsValidated, 100, 0);
    assert(s.window.frames[0] == 1 && s.window.frames[1] == 0 && s.window.frames[2] == 0);
    assert(MLPyrowaveStatsRecent(s.lastDecodedMs, 2011)); // Independent of reset counts.
    memset(&s, 0, sizeof(s));
    MLPyrowaveStatsEvent(&s, 100, MLPyrowaveStatsValidated, 100, 0);
    MLPyrowaveStatsEvent(&s, 1090, MLPyrowaveStatsDecoded, 0, 12);
    MLPyrowaveStatsEvent(&s, 1100, MLPyrowaveStatsPresented, 0, 0);
    MLPyrowaveStatsEvent(&s, 1101, MLPyrowaveStatsPacket, 100, 0);
    assert(s.window.frames[1] == 0 && s.window.frames[2] == 0);
    assert(MLPyrowaveStatsRecent(s.lastDecodedMs, 1101) && MLPyrowaveStatsRecent(s.lastPresentedMs, 1101));
    assert(!MLPyrowaveStatsRecent(s.lastPresentedMs, 2601));
    puts("PyroWave event stats: PASS");
    return 0;
}
