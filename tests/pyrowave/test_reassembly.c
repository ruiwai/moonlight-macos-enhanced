/* Synthetic transport fixtures only: these bytes are NOT codec-validated. */
#include "MLPyrowaveReassembly.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(EXIT_FAILURE); \
} } while (0)
#define R(name) LI_PYROWAVE_REASSEMBLY_##name
#define DATA LI_PYROWAVE_PACKET_DATA
#define HEAD LI_PYROWAVE_PACKET_FRAME_HEADER
#define PARITY LI_PYROWAVE_PACKET_PARITY
static const uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t b[8] = {9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t parity[8] = {8, 8, 8, 8, 8, 8, 8, 24};
static const uint8_t latency[16] = {1, 0, 0, 0, 0, 0, 0, 2, 0x12, 0x34, 1, 2, 3, 4, 5, 6};

static LI_PYROWAVE_PACKET_HEADER header(uint32_t id, uint8_t kind, uint16_t index,
    uint16_t count, uint16_t block, bool fec, uint16_t meta)
{
    LI_PYROWAVE_PACKET_HEADER h = {0};
    h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = 64;
    h.frameId = id; h.rtpTimestamp = 900; h.packetKind = kind;
    h.dataBlockCount = count; h.parityBlockCount = fec ? (count + 15) / 16 : 0;
    h.blockCount = count + h.parityBlockCount; h.blockIndex = index;
    h.fecBlockPayloadSize = block; h.payloadLength = block;
    h.protectedPayloadLength = (uint32_t)count * block;
    h.metadataLength = meta; h.codecPayloadLength = h.protectedPayloadLength - meta;
    h.metadataFlags = meta ? LI_PYROWAVE_METADATA_FLAG_PROTECTED : 0;
    h.flags = meta ? LI_PYROWAVE_FLAG_METADATA_PRESENT : 0;
    h.fecScheme = fec ? LI_PYROWAVE_FEC_SCHEME_XOR : LI_PYROWAVE_FEC_SCHEME_NONE;
    if (kind == HEAD) { h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME; return h; }
    if (fec) {
        h.fecGroupIndex = kind == PARITY ? index - count : index / 16;
        h.fecDataCount = count - h.fecGroupIndex * 16;
        if (h.fecDataCount > 16) { h.fecDataCount = 16; }
        h.fecParityCount = 1;
        h.fecShardIndex = kind == PARITY ? h.fecDataCount : index % 16;
    }
    if (kind == PARITY) { h.flags |= LI_PYROWAVE_FLAG_FEC_PARITY; }
    else {
        if (index == 0) { h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME; }
        if (index + 1 == count) { h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME; }
    }
    return h;
}

static size_t build(LI_PYROWAVE_PACKET_HEADER h, const uint8_t *body, uint8_t *wire)
{
    size_t length = 0;
    CHECK(LiPyrowaveBuildPacket(&h, body, h.payloadLength, wire, 65536, &length) ==
          LI_PYROWAVE_PACKET_OK);
    return length;
}

static LI_PYROWAVE_REASSEMBLY_RESULT push(MLPyrowaveReassembly *ctx,
    LI_PYROWAVE_PACKET_HEADER h, const uint8_t *body, uint64_t now)
{
    uint8_t wire[65536];
    size_t length = build(h, body, wire);
    return MLPyrowaveReassemblyPush(ctx, wire, length, now);
}

static MLPyrowaveFrameInfo copy(MLPyrowaveReassembly *ctx, uint64_t now,
    size_t capacity, LI_PYROWAVE_REASSEMBLY_RESULT expected, const uint8_t *bytes, size_t n)
{
    uint8_t output[64]; size_t length = 99;
    MLPyrowaveFrameInfo info, zero;
    memset(&zero, 0, sizeof(zero));
    memset(output, 0xa5, sizeof(output)); memset(&info, 0xff, sizeof(info));
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, now, output, capacity, &length, &info) == expected);
    CHECK(length == n);
    if (n == 0) { CHECK(memcmp(&info, &zero, sizeof(info)) == 0); }
    else { CHECK(memcmp(output, bytes, n) == 0); }
    for (size_t i = n; i < sizeof(output); ++i) { CHECK(output[i] == 0xa5); }
    return info;
}

static void testReorderCopyReplay(void)
{
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    LI_PYROWAVE_PACKET_HEADER h = header(9, DATA, 1, 2, 8, false, 0);
    CHECK(push(ctx, h, b, 0) == R(ACCEPTED)); /* No FRAME_HEADER. */
    CHECK(push(ctx, h, b, 90000) == R(DUPLICATE));
    (void)copy(ctx, 90000, 64, R(NOT_READY), NULL, 0);
    h = header(9, DATA, 0, 2, 8, false, 0);
    CHECK(push(ctx, h, a, 90001) == R(COMPLETE));
    (void)copy(ctx, 90002, 15, R(OUTPUT_TOO_SMALL), NULL, 0);
    uint8_t expected[16]; memcpy(expected, a, 8); memcpy(expected + 8, b, 8);
    MLPyrowaveFrameInfo info = copy(ctx, 90003, 64, R(COMPLETE), expected, 16);
    CHECK(info.frameId == 9 && info.rtpTimestamp == 900);
    CHECK(info.firstReceiveUs == 0 && info.deadlineUs == 100000);
    CHECK(!info.metadata.hasHostProcessingLatency);
    (void)copy(ctx, 90004, 64, R(NOT_READY), NULL, 0);
    CHECK(push(ctx, h, a, 90005) == R(STALE));
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testDuplicates(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        for (unsigned flags = 0; flags < 2; ++flags) {
            MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
            LI_PYROWAVE_PACKET_HEADER h = header(10, (uint8_t)kind,
                kind == PARITY ? 2 : 0, 2, 8, true, 0);
            CHECK(push(ctx, h, a, 1) == R(ACCEPTED));
            CHECK(push(ctx, h, a, 2) == R(DUPLICATE));
            uint8_t changed[8]; memcpy(changed, a, 8); changed[7] ^= 1;
            if (flags) { h.flags ^= LI_PYROWAVE_FLAG_CRITICAL; }
            CHECK(push(ctx, h, flags ? a : changed, 3) == R(INVALID_PACKET));
            CHECK(push(ctx, h, a, 4) == R(STALE));
            (void)copy(ctx, 4, 64, R(NOT_READY), NULL, 0);
            h = header(11, DATA, 0, 1, 8, false, 0);
            CHECK(push(ctx, h, b, 5) == R(COMPLETE));
            (void)copy(ctx, 5, 64, R(COMPLETE), b, 8);
            MLPyrowaveReassemblyDestroy(ctx);
        }
    }
}

static void testDescriptorsAndPadding(void)
{
    for (unsigned change = 0; change < 7; ++change) {
        MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
        LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 2, 8, true, change == 6 ? 1 : 0);
        CHECK(push(ctx, h, a, 1) == R(ACCEPTED));
        h = header(1, DATA, 1, 2, 8, true, 0);
        if (change == 0) { ++h.rtpTimestamp; }
        if (change == 1) { --h.codecPayloadLength; --h.protectedPayloadLength; }
        if (change == 2) { h = header(1, DATA, 1, 3, 8, true, 0); }
        if (change == 3) { h = header(1, DATA, 1, 2, 7, true, 0); }
        if (change == 4) { h = header(1, DATA, 1, 2, 8, false, 0); }
        if (change == 5) { h = header(1, DATA, 1, 2, 8, true, 1); }
        if (change == 6) { h = header(1, DATA, 1, 2, 8, true, 1);
                          h.metadataFlags |= LI_PYROWAVE_METADATA_FLAG_RUNTIME; }
        CHECK(push(ctx, h, b, 2) == R(INVALID_PACKET));
        CHECK(push(ctx, h, b, 3) == R(STALE));
        MLPyrowaveReassemblyDestroy(ctx);
    }
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 1, 8, false, 0);
    h.codecPayloadLength = h.protectedPayloadLength = 7;
    h.payloadLength = 7; /* Common tooling accepts short final; wrapper does not. */
    CHECK(push(ctx, h, a, 1) == R(INVALID_PACKET));
    h.payloadLength = 8;
    CHECK(push(ctx, h, a, 2) == R(COMPLETE)); /* Invalid first packet set no watermark. */
    uint8_t changed[8]; memcpy(changed, a, 8); changed[7] ^= 1;
    CHECK(push(ctx, h, changed, 3) == R(INVALID_PACKET)); /* Padding also compared. */
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testIds(void)
{
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    LI_PYROWAVE_PACKET_HEADER h = header(UINT32_MAX, DATA, 0, 2, 8, false, 0);
    CHECK(push(ctx, h, a, 1) == R(ACCEPTED));
    h.frameId = UINT32_MAX - 1; CHECK(push(ctx, h, b, 2) == R(STALE));
    h.frameId = UINT32_C(0x7fffffff); CHECK(push(ctx, h, b, 3) == R(STALE));
    h = header(UINT32_MAX, DATA, 1, 2, 8, false, 0);
    CHECK(push(ctx, h, b, 4) == R(COMPLETE)); /* stale did not clear active */
    h = header(1, DATA, 0, 1, 8, false, 0);
    CHECK(push(ctx, h, a, 5) == R(COMPLETE)); /* wrap and replacement */
    (void)copy(ctx, 6, 64, R(COMPLETE), a, 8);
    h.frameId = UINT32_C(0x80000001); CHECK(push(ctx, h, a, 7) == R(STALE));
    h.frameId = 1; CHECK(push(ctx, h, a, 8) == R(STALE));
    h.frameId = 2; CHECK(push(ctx, h, b, 9) == R(COMPLETE));
    (void)copy(ctx, 10, 64, R(COMPLETE), b, 8);
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testDeadlines(void)
{
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 2, 8, false, 0);
    CHECK(push(ctx, h, a, 0) == R(ACCEPTED));
    CHECK(push(ctx, h, a, 99999) == R(DUPLICATE));
    CHECK(push(ctx, h, a, 100000) == R(EXPIRED));
    CHECK(push(ctx, h, a, 100001) == R(STALE));
    h = header(2, DATA, 0, 1, 8, false, 0);
    CHECK(push(ctx, h, a, 100002) == R(COMPLETE));
    (void)copy(ctx, 200002, 64, R(EXPIRED), NULL, 0);
    CHECK(push(ctx, h, a, 200003) == R(STALE));
    h.frameId = 3; CHECK(push(ctx, h, a, UINT64_MAX - 99999) == R(INVALID_PACKET));
    CHECK(push(ctx, h, a, UINT64_MAX - 99998) == R(STALE));
    h.frameId = 4; CHECK(push(ctx, h, a, UINT64_MAX - 99997) == R(INVALID_PACKET));
    MLPyrowaveReassemblyDestroy(ctx);
    ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    CHECK(push(ctx, h, a, UINT64_MAX - 100000) == R(COMPLETE));
    (void)copy(ctx, UINT64_MAX - 1, 64, R(COMPLETE), a, 8);
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testFec(void)
{
    for (unsigned conflict = 0; conflict < 2; ++conflict) {
        MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
        LI_PYROWAVE_PACKET_HEADER h = header(1, PARITY, 2, 2, 8, true, 0);
        CHECK(push(ctx, h, parity, 1) == R(ACCEPTED)); /* two DATA losses */
        (void)copy(ctx, 2, 64, R(NOT_READY), NULL, 0);
        h = header(1, DATA, 0, 2, 8, true, 0);
        CHECK(push(ctx, h, a, 3) == R(COMPLETE)); /* one loss recovered */
        h = header(1, DATA, 1, 2, 8, true, 0);
        CHECK(push(ctx, h, conflict ? a : b, 4) == (conflict ? R(INVALID_PACKET) : R(DUPLICATE)));
        if (conflict) {
            (void)copy(ctx, 5, 64, R(NOT_READY), NULL, 0);
            CHECK(push(ctx, h, b, 6) == R(STALE));
        }
        else {
            uint8_t expected[16]; memcpy(expected, a, 8); memcpy(expected + 8, b, 8);
            (void)copy(ctx, 5, 64, R(COMPLETE), expected, 16);
        }
        MLPyrowaveReassemblyDestroy(ctx);
    }
}

static void testMetadata(void)
{
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    for (unsigned invalid = 0; invalid < 2; ++invalid) {
        uint8_t body[16]; memcpy(body, latency, 16);
        if (invalid) { body[7] = 3; }
        LI_PYROWAVE_PACKET_HEADER h = header(1 + invalid, HEAD, 0, 2, 8, true, 10);
        CHECK(push(ctx, h, b, 1 + invalid * 10) == R(ACCEPTED)); /* NOT authoritative */
        h = header(1 + invalid, DATA, 1, 2, 8, true, 10);
        CHECK(push(ctx, h, body + 8, 2 + invalid * 10) == R(ACCEPTED));
        h = header(1 + invalid, DATA, 0, 2, 8, true, 10);
        uint8_t recoveredParity[8];
        for (unsigned i = 0; i < 8; ++i) { recoveredParity[i] = body[i] ^ body[i + 8]; }
        if (!invalid) { h = header(1, PARITY, 2, 2, 8, true, 10); }
        CHECK(push(ctx, h, invalid ? body : recoveredParity, 3 + invalid * 10) == R(COMPLETE));
        MLPyrowaveFrameInfo info = copy(ctx, 4 + invalid * 10, 64,
            invalid ? R(INVALID_PACKET) : R(COMPLETE), body + 10, invalid ? 0 : 6);
        if (!invalid) { CHECK(info.metadata.hasHostProcessingLatency &&
                             info.metadata.hostProcessingLatency == 0x1234); }
        CHECK(push(ctx, h, body, 5 + invalid * 10) == R(STALE));
    }
    LI_PYROWAVE_PACKET_HEADER h = header(3, DATA, 0, 1, 8, false, 0);
    CHECK(push(ctx, h, a, 30) == R(COMPLETE));
    MLPyrowaveFrameInfo info = copy(ctx, 31, 64, R(COMPLETE), a, 8);
    CHECK(!info.metadata.hasHostProcessingLatency && info.metadata.hostProcessingLatency == 0);
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testMalformedAndBudgets(void)
{
    for (unsigned fault = 0; fault < 5; ++fault) {
        MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
        LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 2, 8, false, 0);
        uint8_t wire[65537]; size_t size = build(h, a, wire);
        CHECK(MLPyrowaveReassemblyPush(ctx, wire, size, 1) == R(ACCEPTED));
        if (fault == 0) { --size; }
        if (fault == 1) { wire[size++] = 0; }
        if (fault == 2) { wire[0] ^= 1; }
        if (fault == 3) { memset(wire + 12, 0, 4); } /* zero ID */
        if (fault == 4) { size = sizeof(wire); }
        CHECK(MLPyrowaveReassemblyPush(ctx, wire, size, 2) == R(INVALID_PACKET));
        CHECK(push(ctx, h, a, 3) == R(STALE));
        (void)copy(ctx, 4, 64, R(NOT_READY), NULL, 0);
        h.frameId = 2; CHECK(push(ctx, h, a, 5) == R(ACCEPTED));
        MLPyrowaveReassemblyDestroy(ctx);
    }
    static const uint8_t large[65472] = {0};
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 513, 32768, false, 0);
    h.codecPayloadLength = h.protectedPayloadLength = 16u * 1024 * 1024 + 1;
    CHECK(push(ctx, h, large, 0) == R(OVERSIZE)); /* preflight before Common */
    CHECK(push(ctx, h, large, 1) == R(STALE));
    h = header(2, HEAD, 0, 65535, 65472, false, 0);
    CHECK(push(ctx, h, large, 2) == R(OVERSIZE)); /* padded/table budgets */
    h = header(3, DATA, 0, 1, 8, false, 0);
    CHECK(push(ctx, h, a, 3) == R(COMPLETE));
    (void)copy(ctx, 4, 64, R(COMPLETE), a, 8);
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testPaddedNoFecLimit(void)
{
    const size_t frameSize = 16777216;
    static uint8_t body[65472];
    uint8_t *output = malloc(frameSize + 16); CHECK(output != NULL);
    memset(output, 0xa5, frameSize + 16);
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    /* Protected bytes hit 16 MiB; 257 full DATA bodies include 49088 padding bytes. */
    for (uint16_t i = 0; i < 257; ++i) {
        for (size_t j = 0; j < sizeof(body); ++j) { body[j] = (uint8_t)(i % 251 + j); }
        LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, i, 257, 65472, false, 0);
        h.codecPayloadLength = h.protectedPayloadLength = (uint32_t)frameSize;
        CHECK(push(ctx, h, body, i) == (i == 256 ? R(COMPLETE) : R(ACCEPTED)));
    }
    size_t length = 0;
    MLPyrowaveFrameInfo info;
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, 257, output, frameSize, &length, &info) ==
          R(COMPLETE));
    CHECK(length == frameSize && info.frameId == 1 && !info.metadata.hasHostProcessingLatency);
    for (size_t i = 0; i < frameSize; ++i) {
        CHECK(output[i] == (uint8_t)((i / sizeof(body)) % 251 + i % sizeof(body)));
    }
    CHECK(output[0] == 0 && output[65471] == 191 && output[65472] == 1);
    CHECK(output[frameSize - 1] == 4); /* Last codec byte, not the padded DATA tail. */
    for (size_t i = frameSize; i < frameSize + 16; ++i) { CHECK(output[i] == 0xa5); }
    free(output);
    MLPyrowaveReassemblyDestroy(ctx);
}

static void testArguments(void)
{
    uint8_t output[8]; size_t length = 99;
    MLPyrowaveFrameInfo info;
    memset(output, 0xa5, sizeof(output)); memset(&info, 0xff, sizeof(info));
    CHECK(MLPyrowaveReassemblyCopyFrame(NULL, 0, output, 8, &length, &info) == R(INVALID_ARGUMENT));
    CHECK(length == 0 && info.frameId == 0 && info.deadlineUs == 0);
    CHECK(MLPyrowaveReassemblyPush(NULL, NULL, 0, 0) == R(INVALID_ARGUMENT));
    MLPyrowaveReassembly *ctx = MLPyrowaveReassemblyCreate(); CHECK(ctx != NULL);
    CHECK(MLPyrowaveReassemblyPush(ctx, NULL, 0, 0) == R(INVALID_PACKET));
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, 0, NULL, 8, &length, &info) == R(INVALID_ARGUMENT));
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, 0, output, 8, NULL, &info) == R(INVALID_ARGUMENT));
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, 0, output, 8, &length, NULL) == R(INVALID_ARGUMENT));
    LI_PYROWAVE_PACKET_HEADER h = header(1, DATA, 0, 1, 8, false, 0);
    CHECK(push(ctx, h, a, 1) == R(COMPLETE)); /* invalid first wire set no watermark */
    CHECK(MLPyrowaveReassemblyCopyFrame(ctx, 2, output, SIZE_MAX, &length, &info) == R(OVERSIZE));
    CHECK(length == 0 && info.frameId == 0);
    for (size_t i = 0; i < sizeof(output); ++i) { CHECK(output[i] == 0xa5); }
    (void)copy(ctx, 3, 8, R(COMPLETE), a, 8);
    MLPyrowaveReassemblyDestroy(ctx);
}

int main(void)
{
    testReorderCopyReplay(); testDuplicates(); testDescriptorsAndPadding(); testIds();
    testDeadlines(); testFec(); testMetadata(); testMalformedAndBudgets(); testArguments();
    testPaddedNoFecLimit();
    MLPyrowaveReassemblyDestroy(NULL);
    puts("PyroWave strict reassembly checks passed");
    return EXIT_SUCCESS;
}
