#include "MLPyrowaveReassembly.h"

#include <stdlib.h>
#include <string.h>

#define MIB (UINT64_C(1024) * 1024)
#define FRAME_LIMIT (16 * MIB)
#define COMMON_PAYLOAD_LIMIT (40 * MIB)
#define SAVED_LIMIT (48 * MIB)
#define TOTAL_LIMIT (128 * MIB)
/* Conservative per-allocation accounting allowance, not allocator introspection. */
#define ALLOC_OVERHEAD UINT64_C(64)
#define DEADLINE_US UINT64_C(100000)

typedef struct SavedPacket {
    uint8_t *bytes;
    size_t length;
} SavedPacket;

struct MLPyrowaveReassembly {
    LI_PYROWAVE_REASSEMBLY_STATE common;
    LI_PYROWAVE_PACKET_HEADER descriptor;
    SavedPacket *saved; /* blockCount slots plus one separate FRAME_HEADER. */
    uint8_t *storage; /* Bounded high-water allocation retained until destroy. */
    size_t capacity, used;
    uint32_t lastId;
    bool active;
    uint64_t firstUs, deadlineUs;
    uint64_t savedBytes, commonBudget, parityBytes;
};

static LI_PYROWAVE_REASSEMBLY_RESULT retire(
    MLPyrowaveReassembly *ctx, LI_PYROWAVE_REASSEMBLY_RESULT result)
{
    ctx->saved = NULL; /* Poison logical aliases, not retained arena capacity. */
    ctx->used = 0;
    ctx->firstUs = ctx->deadlineUs = 0;
    LiPyrowaveReassemblyReset(&ctx->common);
    ctx->active = false;
    ctx->savedBytes = ctx->commonBudget = ctx->parityBytes = 0;
    return result; /* lastId deliberately survives every retirement. */
}

void MLPyrowaveReassemblyDiscardFrame(MLPyrowaveReassembly *ctx)
{
    if (ctx != NULL) (void)retire(ctx, LI_PYROWAVE_REASSEMBLY_ACCEPTED);
}

static bool sameDescriptor(const LI_PYROWAVE_PACKET_HEADER *a,
                           const LI_PYROWAVE_PACKET_HEADER *b)
{
    return a->frameId == b->frameId && a->rtpTimestamp == b->rtpTimestamp &&
        a->codecPayloadLength == b->codecPayloadLength &&
        a->protectedPayloadLength == b->protectedPayloadLength &&
        a->metadataLength == b->metadataLength && a->metadataFlags == b->metadataFlags &&
        a->fecScheme == b->fecScheme && a->fecBlockPayloadSize == b->fecBlockPayloadSize &&
        a->dataBlockCount == b->dataBlockCount && a->parityBlockCount == b->parityBlockCount &&
        a->blockCount == b->blockCount;
}

static bool fitsTotal(uint64_t saved, uint64_t common)
{
    return saved + common + FRAME_LIMIT + LI_PYROWAVE_MAX_METADATA_SIZE +
        sizeof(MLPyrowaveReassembly) + 2 * ALLOC_OVERHEAD < TOTAL_LIMIT;
}

static bool budgets(const LI_PYROWAVE_PACKET_HEADER *h,
                    uint64_t *savedTable, uint64_t *commonBudget)
{
    const uint64_t count = h->blockCount;
    const uint64_t body = count * h->fecBlockPayloadSize;
    const uint64_t table = (count + 1) * sizeof(SavedPacket) + ALLOC_OVERHEAD;
    const uint64_t saved = table + (count + 1) *
        (LI_PYROWAVE_WIRE_HEADER_SIZE + h->fecBlockPayloadSize + ALLOC_OVERHEAD);
    /* Common owns data/parity pointer, uint16 length, uint8 received tables;
     * at most six table allocations and one allocation per wire block,
     * including recovered DATA. No Common streaming buffer is used.
     */
    const uint64_t common = body + count * (sizeof(uint8_t *) + sizeof(uint16_t) +
        sizeof(uint8_t) + ALLOC_OVERHEAD) + 6 * ALLOC_OVERHEAD;
    *savedTable = table;
    *commonBudget = common;
    return h->protectedPayloadLength <= FRAME_LIMIT && body <= COMMON_PAYLOAD_LIMIT &&
        saved <= SAVED_LIMIT && fitsTotal(saved, common);
}

MLPyrowaveReassembly *MLPyrowaveReassemblyCreate(void)
{
    if (sizeof(size_t) != 8 || sizeof(void *) != 8) {
        return NULL;
    }
    MLPyrowaveReassembly *ctx = calloc(1, sizeof(*ctx));
    if (ctx != NULL) {
        /* Common counts padded no-FEC DATA; budgets() caps protected bytes separately. */
        LiPyrowaveReassemblyInitialize(&ctx->common, (uint32_t)COMMON_PAYLOAD_LIMIT);
    }
    return ctx;
}

void MLPyrowaveReassemblyDestroy(MLPyrowaveReassembly *ctx)
{
    if (ctx != NULL) {
        (void)retire(ctx, LI_PYROWAVE_REASSEMBLY_ACCEPTED);
        free(ctx->storage); /* Retirement retains storage; destruction releases it. */
        free(ctx);
    }
}

static LI_PYROWAVE_REASSEMBLY_RESULT begin(
    MLPyrowaveReassembly *ctx, const LI_PYROWAVE_PACKET_HEADER *h, uint64_t nowUs)
{
    uint64_t table, common;
    (void)retire(ctx, LI_PYROWAVE_REASSEMBLY_ACCEPTED);
    ctx->lastId = h->frameId; /* Even rejected allocation/overflow cannot reopen. */
    ctx->descriptor = *h;
    if (!budgets(h, &table, &common)) {
        return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
    }
    if (nowUs > UINT64_MAX - DEADLINE_US) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_PACKET;
    }
    const uint64_t tableBytes = ((uint64_t)h->blockCount + 1) * sizeof(SavedPacket);
    const uint64_t required = tableBytes + ((uint64_t)h->blockCount + 1) *
        (LI_PYROWAVE_WIRE_HEADER_SIZE + h->fecBlockPayloadSize);
    /* Charge the full retained high-water capacity, even for a smaller frame. */
    if (ctx->capacity < required || !fitsTotal(ctx->capacity + ALLOC_OVERHEAD, common)) {
        free(ctx->storage); /* Free before growth: never overlap two arenas. */
        ctx->storage = NULL;
        ctx->capacity = 0;
        ctx->storage = malloc((size_t)required);
        if (ctx->storage == NULL) {
            return LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY;
        }
        ctx->capacity = (size_t)required;
    }
    memset(ctx->storage, 0, (size_t)tableBytes); /* Only the active table prefix. */
    ctx->saved = (SavedPacket *)ctx->storage;
    ctx->used = (size_t)tableBytes;
    ctx->savedBytes = table;
    ctx->commonBudget = common;
    ctx->firstUs = nowUs;
    ctx->deadlineUs = nowUs + DEADLINE_US;
    ctx->active = true;
    return LI_PYROWAVE_REASSEMBLY_ACCEPTED;
}

/* Common silently returns from a failed recovered-block malloc. If parity and
 * exactly one missing DATA remain after Push, that allocation failed. */
static bool failedRecovery(const LI_PYROWAVE_REASSEMBLY_STATE *s, uint16_t group)
{
    if (!s->fecEnabled || s->fecParityReceived[group] == 0) {
        return false;
    }
    unsigned missing = 0;
    const uint32_t start = (uint32_t)group * LI_PYROWAVE_FEC_DATA_PER_GROUP;
    for (uint32_t i = start; i < s->blockCount &&
         i < start + LI_PYROWAVE_FEC_DATA_PER_GROUP; ++i) {
        missing += s->received[i] == 0;
    }
    return missing == 1;
}

static LI_PYROWAVE_REASSEMBLY_RESULT pushCommon(
    MLPyrowaveReassembly *ctx, const LI_PYROWAVE_PACKET_HEADER *h,
    const uint8_t *payload, uint64_t nowUs)
{
    LI_PYROWAVE_REASSEMBLY_STATE *s = &ctx->common;
    /* No saved original for this DATA: Common may already own recovered bytes.
     * Compare BEFORE Common's duplicate fast path, including padding. */
    if (h->packetKind == LI_PYROWAVE_PACKET_DATA && s->blocks != NULL &&
        s->received[h->blockIndex] != 0 &&
        (s->blockLengths[h->blockIndex] != h->payloadLength ||
         memcmp(s->blocks[h->blockIndex], payload, h->payloadLength) != 0)) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
    }
    LI_PYROWAVE_REASSEMBLY_RESULT r = LiPyrowaveReassemblyPush(
        s, h, payload, nowUs, ctx->deadlineUs);
    if (r < 0) {
        return retire(ctx, r);
    }
    if (h->packetKind == LI_PYROWAVE_PACKET_PARITY) {
        ctx->parityBytes += h->payloadLength;
    }
    if (h->packetKind != LI_PYROWAVE_PACKET_FRAME_HEADER &&
        failedRecovery(s, h->fecGroupIndex)) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY);
    }
    const uint64_t used = s->totalPayloadLength + ctx->parityBytes +
        (uint64_t)h->blockCount * (sizeof(uint8_t *) + sizeof(uint16_t) +
        sizeof(uint8_t) + ALLOC_OVERHEAD) + 6 * ALLOC_OVERHEAD;
    if (used > ctx->commonBudget) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_OVERSIZE);
    }
    return r;
}

LI_PYROWAVE_REASSEMBLY_RESULT MLPyrowaveReassemblyPush(
    MLPyrowaveReassembly *ctx, const uint8_t *packet, size_t size, uint64_t nowUs)
{
    LI_PYROWAVE_PACKET_HEADER h;
    const uint8_t *payload;
    if (ctx == NULL) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    if (size > LI_PYROWAVE_MAX_PACKET_SIZE ||
        LiPyrowaveParsePacket(packet, size, &h, &payload) != LI_PYROWAVE_PACKET_OK ||
        h.payloadLength != h.fecBlockPayloadSize) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
    }
    if (h.frameId != ctx->lastId) {
        const uint32_t diff = h.frameId - ctx->lastId;
        if (ctx->lastId != 0 && (diff == 0 || diff >= UINT32_C(0x80000000))) {
            return LI_PYROWAVE_REASSEMBLY_STALE;
        }
        LI_PYROWAVE_REASSEMBLY_RESULT r = begin(ctx, &h, nowUs);
        if (r != LI_PYROWAVE_REASSEMBLY_ACCEPTED) {
            return r;
        }
    }
    else if (!ctx->active) {
        return LI_PYROWAVE_REASSEMBLY_STALE;
    }
    if (nowUs >= ctx->deadlineUs) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_EXPIRED);
    }
    if (!sameDescriptor(&h, &ctx->descriptor)) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
    }
    const uint32_t slot = h.packetKind == LI_PYROWAVE_PACKET_FRAME_HEADER
        ? h.blockCount : h.blockIndex;
    SavedPacket *saved = &ctx->saved[slot];
    if (saved->bytes != NULL) {
        if (saved->length != size || memcmp(saved->bytes, packet, size) != 0) {
            return retire(ctx, LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
        }
        return LI_PYROWAVE_REASSEMBLY_DUPLICATE;
    }
    if (size + ALLOC_OVERHEAD > SAVED_LIMIT - ctx->savedBytes ||
        ctx->used > ctx->capacity || size > ctx->capacity - ctx->used) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_OVERSIZE);
    }
    saved->bytes = ctx->storage + ctx->used;
    memcpy(saved->bytes, packet, size);
    saved->length = size;
    ctx->used += size;
    ctx->savedBytes += size + ALLOC_OVERHEAD;
    return pushCommon(ctx, &h, payload, nowUs);
}

LI_PYROWAVE_REASSEMBLY_RESULT MLPyrowaveReassemblyCopyFrame(
    MLPyrowaveReassembly *ctx, uint64_t nowUs, uint8_t *output, size_t capacity,
    size_t *length, MLPyrowaveFrameInfo *info)
{
    if (length != NULL) {
        *length = 0;
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (ctx == NULL || output == NULL || length == NULL || info == NULL) {
        return LI_PYROWAVE_REASSEMBLY_INVALID_ARGUMENT;
    }
    if (ctx->active && nowUs >= ctx->deadlineUs) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_EXPIRED);
    }
    if (!ctx->active || !LiPyrowaveReassemblyIsComplete(&ctx->common)) {
        return LI_PYROWAVE_REASSEMBLY_NOT_READY;
    }
    if (capacity > FRAME_LIMIT) {
        return LI_PYROWAVE_REASSEMBLY_OVERSIZE;
    }
    if (capacity < ctx->descriptor.codecPayloadLength) {
        return LI_PYROWAVE_REASSEMBLY_OUTPUT_TOO_SMALL;
    }
    const size_t metaSize = ctx->descriptor.metadataLength;
    uint8_t *metadata = metaSize != 0 ? malloc(metaSize) : NULL;
    if (metaSize != 0 && metadata == NULL) {
        return retire(ctx, LI_PYROWAVE_REASSEMBLY_OUT_OF_MEMORY);
    }
    size_t metaLength = 0;
    uint16_t flags = 0;
    MLPyrowaveMetadata snapshot;
    LI_PYROWAVE_REASSEMBLY_RESULT r = LiPyrowaveReassemblyCopyMetadata(
        &ctx->common, metadata, metaSize, &metaLength, &flags);
    const bool valid = r == LI_PYROWAVE_REASSEMBLY_COMPLETE && metaLength == metaSize &&
        flags == ctx->descriptor.metadataFlags &&
        MLPyrowaveValidateMetadata(metadata, metaLength, flags, &snapshot);
    free(metadata);
    if (!valid) {
        return retire(ctx, r < 0 ? r : LI_PYROWAVE_REASSEMBLY_INVALID_PACKET);
    }
    r = LiPyrowaveReassemblyCopyFrame(&ctx->common, output, capacity, length, NULL);
    if (r == LI_PYROWAVE_REASSEMBLY_COMPLETE) {
        info->frameId = ctx->lastId;
        info->rtpTimestamp = ctx->descriptor.rtpTimestamp;
        info->firstReceiveUs = ctx->firstUs;
        info->deadlineUs = ctx->deadlineUs;
        info->metadata = snapshot;
    }
    else {
        *length = 0;
    }
    return retire(ctx, r);
}
