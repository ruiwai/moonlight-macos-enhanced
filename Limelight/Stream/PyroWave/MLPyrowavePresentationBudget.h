// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_PRESENTATION_BUDGET_H
#define ML_PYROWAVE_PRESENTATION_BUDGET_H
#include <stdint.h>

// Pure geometry, not a memory proof: scale=min(1, sourceW/backingW, sourceH/backingH).
// Select the limiting axis directly; floor error versus the ideal canvas is <1 pixel/axis.
static inline int MLPyrowaveSelectDrawableSize(uint32_t sourceW, uint32_t sourceH,
    uint32_t backingW, uint32_t backingH, uint32_t *outW, uint32_t *outH)
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!outW || !outH || !sourceW || !sourceH || !backingW || !backingH ||
        sourceW > 16384 || sourceH > 16384 || backingW > 16384 || backingH > 16384)
        return 0;
    uint32_t w = backingW, h = backingH;
    if (sourceW < backingW && (uint64_t)sourceW * backingH <= (uint64_t)sourceH * backingW) {
        w = sourceW;
        h = (uint32_t)((uint64_t)backingH * sourceW / backingW);
    } else if (sourceH < backingH) {
        h = sourceH;
        w = (uint32_t)((uint64_t)backingW * sourceH / backingH);
    }
    if (!w || !h) return 0;
    *outW = w; *outH = h;
    return 1;
}

static const uint64_t MLPyrowaveSessionAllocationCap = UINT64_C(256) * 1024 * 1024;
// Allocation charges only, not RSS. Unknown/zero charges are not admissible.
static inline int MLPyrowavePresentationFitsBudget(uint64_t sessionBytes,
    uint64_t textureBytes, uint64_t textureCount)
{
    return sessionBytes && textureBytes && textureCount &&
        sessionBytes <= MLPyrowaveSessionAllocationCap &&
        textureCount <= (MLPyrowaveSessionAllocationCap - sessionBytes) / textureBytes;
}
#endif
