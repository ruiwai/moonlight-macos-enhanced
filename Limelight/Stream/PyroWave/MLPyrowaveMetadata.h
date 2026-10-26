#ifndef ML_PYROWAVE_METADATA_H
#define ML_PYROWAVE_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MLPyrowaveMetadata {
    bool hasHostProcessingLatency;
    uint16_t hostProcessingLatency; /* 0.1 ms units, host-endian. */
} MLPyrowaveMetadata;

/*
 * SDR-only validation of recovered protected metadata, not codec bytes or a
 * frame-header copy. The caller validates envelope presence and geometry.
 * Uses the pinned Common PyrowaveProtocol.h contract; allocates nothing and
 * retains no pointers. Output is required and cleared before validation;
 * only a completely valid region publishes a fresh snapshot.
 * Nonempty regions require envelope PROTECTED, not per-entry PROTECTED.
 * Unknown/reserved entries skip unless entry or envelope is REQUIRED;
 * known dynamic HDR/HLG entries are unsupported even when OPTIONAL.
 */
bool MLPyrowaveValidateMetadata(const uint8_t *data, size_t length,
                              uint16_t envelopeFlags, MLPyrowaveMetadata *output);

#ifdef __cplusplus
}
#endif

#endif
