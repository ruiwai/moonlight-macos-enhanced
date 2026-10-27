// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_PRESENTATION_BUDGET_H
#define ML_PYROWAVE_PRESENTATION_BUDGET_H
#include <stdint.h>

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
