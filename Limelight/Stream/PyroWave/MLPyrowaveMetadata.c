#include "MLPyrowaveMetadata.h"
#include "PyrowaveProtocol.h"

#include <string.h>

static uint16_t readBE16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t readBE32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static bool validFlags(uint16_t flags)
{
    const uint16_t mask = LI_PYROWAVE_METADATA_FLAG_PROTECTED |
                          LI_PYROWAVE_METADATA_FLAG_RUNTIME |
                          LI_PYROWAVE_METADATA_FLAG_OPTIONAL |
                          LI_PYROWAVE_METADATA_FLAG_REQUIRED;
    return (flags & ~mask) == 0 &&
           !((flags & LI_PYROWAVE_METADATA_FLAG_OPTIONAL) != 0 &&
             (flags & LI_PYROWAVE_METADATA_FLAG_REQUIRED) != 0);
}

static bool unsupportedDynamicType(uint16_t type)
{
    return type == LI_PYROWAVE_METADATA_HDR10_PLUS ||
           type == LI_PYROWAVE_METADATA_HDR_VIVID ||
           type == LI_PYROWAVE_METADATA_DOLBY_VISION_RPU ||
           type == LI_PYROWAVE_METADATA_HLG_NOMINAL_PEAK;
}

bool MLPyrowaveValidateMetadata(const uint8_t *data, size_t length,
                              uint16_t envelopeFlags, MLPyrowaveMetadata *output)
{
    MLPyrowaveMetadata snapshot = {0};
    if (output == NULL) {
        return false;
    }
    memset(output, 0, sizeof(*output));
    if (length > LI_PYROWAVE_MAX_METADATA_SIZE || (data == NULL && length != 0)) {
        return false;
    }
    if (length == 0) {
        return envelopeFlags == 0;
    }
    if (!validFlags(envelopeFlags) ||
        (envelopeFlags & LI_PYROWAVE_METADATA_FLAG_PROTECTED) == 0) {
        return false;
    }
    while (length != 0) {
        if (length < 8) {
            return false;
        }
        const uint16_t type = readBE16(data);
        const uint16_t flags = readBE16(data + 2);
        const uint32_t payloadLength = readBE32(data + 4);
        data += 8;
        length -= 8;
        if (!validFlags(flags) || payloadLength > length) {
            return false;
        }
        if (type == LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY) {
            if (payloadLength != 2 || snapshot.hasHostProcessingLatency) {
                return false;
            }
            snapshot.hasHostProcessingLatency = true;
            snapshot.hostProcessingLatency = readBE16(data);
        }
        else if (unsupportedDynamicType(type) ||
                 ((flags | envelopeFlags) & LI_PYROWAVE_METADATA_FLAG_REQUIRED) != 0) {
            return false;
        }
        data += payloadLength;
        length -= payloadLength;
    }
    *output = snapshot;
    return true;
}
