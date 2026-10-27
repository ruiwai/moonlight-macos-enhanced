#ifndef ML_PYROWAVE_REASSEMBLY_H
#define ML_PYROWAVE_REASSEMBLY_H

#include "MLPyrowaveMetadata.h"
#include "PyrowaveReassembly.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MLPyrowaveReassembly MLPyrowaveReassembly;
typedef struct MLPyrowaveFrameInfo {
    uint32_t frameId;
    uint32_t rtpTimestamp;
    uint64_t firstReceiveUs;
    uint64_t deadlineUs;
    MLPyrowaveMetadata metadata;
} MLPyrowaveFrameInfo;

/* Standalone, disabled transport adapter; no codec validation or live wiring.
 * Serialized caller, monotonic microsecond clock (zero is valid). One frame,
 * first arrival +100000 us, exact deadline expires. No fragments/concatenation.
 * Supported platform: 64-bit pointers/size_t. Protected bytes <=16 MiB;
 * padded DATA required even with no FEC (tooling only, not negotiation).
 * Conflicts/malformed packets poison the active frame. Retired IDs cannot
 * reopen; only modularly newer IDs (difference <2^31) can start another.
 * Create can return NULL; Destroy accepts NULL and releases retained storage.
 * No retained caller pointers.
 */
MLPyrowaveReassembly *MLPyrowaveReassemblyCreate(void);
void MLPyrowaveReassemblyDestroy(MLPyrowaveReassembly *ctx);
/* NULL-safe logical frame poison; preserves replay watermark and bounded storage capacity. */
void MLPyrowaveReassemblyDiscardFrame(MLPyrowaveReassembly *ctx);
LI_PYROWAVE_REASSEMBLY_RESULT MLPyrowaveReassemblyPush(
    MLPyrowaveReassembly *ctx, const uint8_t *packet, size_t size, uint64_t nowUs);

/* All pointers required. Clears length/info on entry, never output bytes until
 * final Common copy after complete recovery and protected metadata validation.
 * Output capacity <=16 MiB; caller owns output and info, with no aliasing.
 * OUTPUT_TOO_SMALL retains a valid frame for retry before expiry; successful
 * copy retires it. Copied pure codec bytes STILL REQUIRE a codec validator!
 * FrameInfo owns scalars, not pointers; timestamp is not a wall clock.
 */
LI_PYROWAVE_REASSEMBLY_RESULT MLPyrowaveReassemblyCopyFrame(
    MLPyrowaveReassembly *ctx, uint64_t nowUs, uint8_t *output, size_t capacity,
    size_t *length, MLPyrowaveFrameInfo *info);

#ifdef __cplusplus
}
#endif
#endif
