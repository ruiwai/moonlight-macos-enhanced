/* Explicit synthetic TLV fixtures, not real encoder/bitstream provenance. */
#include "MLPyrowaveMetadata.h"
#include "PyrowaveProtocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

enum {
    PROTECTED = LI_PYROWAVE_METADATA_FLAG_PROTECTED,
    RUNTIME = LI_PYROWAVE_METADATA_FLAG_RUNTIME,
    OPTIONAL = LI_PYROWAVE_METADATA_FLAG_OPTIONAL,
    REQUIRED = LI_PYROWAVE_METADATA_FLAG_REQUIRED
};
static const uint8_t latency[] = {1, 0, 0, 0, 0, 0, 0, 2, 0x12, 0x34};
static const uint8_t unknown[] = {0x7f, 0xff, 0, OPTIONAL, 0, 0, 0, 1, 0xab};

static void expect(const uint8_t *data, size_t length, uint16_t flags,
                   bool success, bool present, uint16_t value)
{
    MLPyrowaveMetadata output = {true, 0xbeef};
    CHECK(MLPyrowaveValidateMetadata(data, length, flags, &output) == success);
    CHECK(output.hasHostProcessingLatency == present);
    CHECK(output.hostProcessingLatency == value);
}

static void testArgumentsAndReset(void)
{
    MLPyrowaveMetadata output = {true, 99};
    CHECK(!MLPyrowaveValidateMetadata(NULL, 0, 0, NULL));
    CHECK(!MLPyrowaveValidateMetadata(latency, sizeof(latency), PROTECTED, NULL));
    expect(NULL, 1, PROTECTED, false, false, 0);
    expect(NULL, 0, 0, true, false, 0);
    expect(latency, 0, 0, true, false, 0);
    for (unsigned bit = 0; bit < 16; ++bit) {
        expect(NULL, 0, (uint16_t)(1u << bit), false, false, 0);
    }
    /* The bound must be checked before attempting to read this tiny buffer. */
    expect(latency, (size_t)LI_PYROWAVE_MAX_METADATA_SIZE + 1, PROTECTED, false, false, 0);
    expect(latency, SIZE_MAX, PROTECTED, false, false, 0);
    CHECK(MLPyrowaveValidateMetadata(latency, sizeof(latency), PROTECTED, &output));
    CHECK(output.hasHostProcessingLatency && output.hostProcessingLatency == 0x1234);
    CHECK(MLPyrowaveValidateMetadata(unknown, sizeof(unknown), PROTECTED, &output));
    CHECK(!output.hasHostProcessingLatency && output.hostProcessingLatency == 0);
    CHECK(MLPyrowaveValidateMetadata(latency, sizeof(latency), PROTECTED, &output));
    CHECK(!MLPyrowaveValidateMetadata(latency, 9, PROTECTED, &output));
    CHECK(!output.hasHostProcessingLatency && output.hostProcessingLatency == 0);
    CHECK(MLPyrowaveValidateMetadata(latency, sizeof(latency), PROTECTED, &output));
    CHECK(MLPyrowaveValidateMetadata(NULL, 0, 0, &output));
    CHECK(!output.hasHostProcessingLatency && output.hostProcessingLatency == 0);
}

static void testLatencyAndLengths(void)
{
    _Alignas(uint32_t) uint8_t bytes[11] = {0};
    memcpy(bytes, latency, sizeof(latency));
    expect(bytes, 10, PROTECTED, true, true, 0x1234);
    bytes[8] = bytes[9] = 0;
    expect(bytes, 10, PROTECTED, true, true, 0);
    bytes[8] = bytes[9] = 0xff;
    expect(bytes, 10, PROTECTED, true, true, UINT16_MAX);
    /* Every truncated prefix, including zero bytes with nonzero flags. */
    for (size_t length = 0; length < sizeof(latency); ++length) {
        expect(latency, length, PROTECTED, false, false, 0);
    }
    const uint8_t badLengths[] = {0, 1, 3};
    for (size_t i = 0; i < sizeof(badLengths); ++i) {
        bytes[7] = badLengths[i];
        expect(bytes, 8 + badLengths[i], PROTECTED, false, false, 0);
    }
    memset(bytes + 4, 0xff, 4); /* UINT32_MAX payload length. */
    expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
    memcpy(bytes + 1, latency, sizeof(latency));
    expect(bytes + 1, sizeof(latency), PROTECTED, true, true, 0x1234);
}

static void testFlags(void)
{
    uint8_t bytes[sizeof(latency)];
    memcpy(bytes, latency, sizeof(bytes));
    for (unsigned flags = 0; flags < 16; ++flags) {
        const bool valid = !((flags & OPTIONAL) && (flags & REQUIRED));
        bytes[3] = (uint8_t)flags;
        expect(bytes, sizeof(bytes), PROTECTED, valid, valid, valid ? 0x1234 : 0);
        const bool envelopeValid = valid && (flags & PROTECTED);
        expect(latency, sizeof(latency), (uint16_t)flags,
               envelopeValid, envelopeValid, envelopeValid ? 0x1234 : 0);
    }
    for (unsigned bit = 4; bit < 16; ++bit) {
        const uint16_t badFlag = (uint16_t)(1u << bit);
        expect(latency, sizeof(latency), PROTECTED | badFlag, false, false, 0);
        bytes[2] = (uint8_t)(badFlag >> 8);
        bytes[3] = (uint8_t)badFlag;
        expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
    }
}

static void testUnsupportedTypes(void)
{
    uint8_t bytes[sizeof(unknown)];
    memcpy(bytes, unknown, sizeof(bytes));
    expect(bytes, sizeof(bytes), PROTECTED, true, false, 0);
    bytes[3] = 0; /* Flagless unknown entries follow Common's skip policy. */
    expect(bytes, sizeof(bytes), PROTECTED, true, false, 0);
    expect(bytes, sizeof(bytes), PROTECTED | REQUIRED, false, false, 0);
    bytes[3] = REQUIRED;
    expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
    bytes[3] = OPTIONAL;
    expect(bytes, sizeof(bytes), PROTECTED | REQUIRED, false, false, 0);
    const uint16_t reserved[] = {LI_PYROWAVE_METADATA_HDR_STATIC_SNAPSHOT,
        LI_PYROWAVE_METADATA_COLOR_CONTRACT, LI_PYROWAVE_METADATA_FRAME_DEADLINE,
        LI_PYROWAVE_METADATA_TRANSPORT_STATUS};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        bytes[0] = (uint8_t)(reserved[i] >> 8);
        bytes[1] = (uint8_t)reserved[i];
        bytes[3] = OPTIONAL;
        expect(bytes, sizeof(bytes), PROTECTED, true, false, 0);
        bytes[3] = REQUIRED;
        expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
        bytes[3] = 0;
        expect(bytes, sizeof(bytes), PROTECTED | REQUIRED, false, false, 0);
    }
    const uint8_t dynamic[] = {LI_PYROWAVE_METADATA_HDR10_PLUS,
        LI_PYROWAVE_METADATA_HDR_VIVID, LI_PYROWAVE_METADATA_DOLBY_VISION_RPU,
        LI_PYROWAVE_METADATA_HLG_NOMINAL_PEAK};
    for (size_t i = 0; i < sizeof(dynamic); ++i) {
        bytes[0] = 0;
        bytes[1] = dynamic[i];
        for (unsigned flags = 0; flags < 16; ++flags) {
            bytes[3] = (uint8_t)flags;
            expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
        }
    }
    memcpy(bytes, unknown, sizeof(bytes));
    bytes[7] = 2; /* Unknown type also rejects a payload beyond remaining. */
    expect(bytes, sizeof(bytes), PROTECTED, false, false, 0);
}

static void testAtomicPublication(void)
{
    uint8_t bytes[2 * sizeof(latency)];
    memcpy(bytes, latency, sizeof(latency));
    memcpy(bytes + sizeof(latency), latency, sizeof(latency));
    expect(bytes, sizeof(bytes), PROTECTED, false, false, 0); /* Identical duplicate. */
    bytes[sizeof(bytes) - 1] ^= 1;
    expect(bytes, sizeof(bytes), PROTECTED, false, false, 0); /* Conflicting duplicate. */
    for (size_t suffix = 1; suffix < sizeof(latency); ++suffix) {
        expect(bytes, sizeof(latency) + suffix, PROTECTED, false, false, 0);
    }
    memcpy(bytes + sizeof(latency), unknown, sizeof(unknown));
    expect(bytes, sizeof(latency) + sizeof(unknown), PROTECTED, true, true, 0x1234);
    bytes[sizeof(latency) + 3] = REQUIRED;
    expect(bytes, sizeof(latency) + sizeof(unknown), PROTECTED, false, false, 0);
}

static void testMaximumRegion(void)
{
    /* 65535 total bytes: optional unknown header + 65527 opaque bytes. */
    static uint8_t bytes[LI_PYROWAVE_MAX_METADATA_SIZE] =
        {0x7f, 0xff, 0, OPTIONAL, 0, 0, 0xff, 0xf7};
    expect(bytes, sizeof(bytes), PROTECTED | OPTIONAL, true, false, 0);
    expect(bytes, sizeof(bytes) - 1, PROTECTED, false, false, 0);
}

int main(void)
{
    testArgumentsAndReset();
    testLatencyAndLengths();
    testFlags();
    testUnsupportedTypes();
    testAtomicPublication();
    testMaximumRegion();
    puts("PyroWave metadata checks passed");
    return EXIT_SUCCESS;
}
