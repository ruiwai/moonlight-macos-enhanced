/* CI-only native adapter allocation accounting, not Common/receive-path counts. */
#include <stdio.h>
#include <stdlib.h> /* Must precede allocator macros. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(EXIT_FAILURE); \
} } while (0)
static struct { void *pointer; size_t size; } owned[8];
static size_t live, peak, liveCount, mallocs, callocs, frees, entryLive;
static size_t expectEntry = SIZE_MAX;
static bool failMalloc, failCalloc;
static void *track(void *p, size_t n)
{
    CHECK(p != NULL);
    for (size_t i = 0; i < 8; ++i) if (owned[i].pointer == NULL) {
        owned[i].pointer = p; owned[i].size = n; live += n; ++liveCount;
        if (live > peak) peak = live;
        return p;
    }
    CHECK(false); return NULL;
}
static void *countMalloc(size_t n)
{
    ++mallocs; entryLive = live;
    if (expectEntry != SIZE_MAX) { CHECK(live == expectEntry && liveCount == 1); }
    if (failMalloc) { failMalloc = false; return NULL; }
    return track(malloc(n), n);
}
static void *countCalloc(size_t n, size_t size)
{
    ++callocs;
    if (failCalloc) { failCalloc = false; return NULL; }
    CHECK(size == 0 || n <= SIZE_MAX / size);
    return track(calloc(n, size), n * size);
}
static void countFree(void *p)
{
    if (p == NULL) return;
    for (size_t i = 0; i < 8; ++i) if (owned[i].pointer == p) {
        live -= owned[i].size; --liveCount; ++frees;
        owned[i].pointer = NULL; free(p); return;
    }
    CHECK(false); /* Common allocations must never enter these wrappers. */
}
#define malloc countMalloc
#define calloc countCalloc
#define free countFree
#include "../../Limelight/Stream/PyroWave/MLPyrowaveReassembly.c"
#undef malloc
#undef calloc
#undef free
#define R(n) LI_PYROWAVE_REASSEMBLY_##n
static size_t wire(uint8_t *out, uint32_t id, uint16_t count, uint16_t block,
                   uint16_t index, uint8_t value, bool head)
{
    static uint8_t body[65472];
    LI_PYROWAVE_PACKET_HEADER h = {0}; size_t n = 0;
    h.version = LI_PYROWAVE_PROTOCOL_VERSION; h.headerLength = 64;
    h.frameId = id; h.rtpTimestamp = 900;
    h.packetKind = head ? LI_PYROWAVE_PACKET_FRAME_HEADER : LI_PYROWAVE_PACKET_DATA;
    h.blockCount = h.dataBlockCount = count; h.blockIndex = index;
    h.fecBlockPayloadSize = h.payloadLength = block;
    h.codecPayloadLength = h.protectedPayloadLength = (uint32_t)count * block;
    h.fecScheme = LI_PYROWAVE_FEC_SCHEME_NONE;
    if (head || index == 0) h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
    if (!head && index + 1 == count) h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
    memset(body, value, block);
    CHECK(LiPyrowaveBuildPacket(&h, body, block, out, 65536, &n) == LI_PYROWAVE_PACKET_OK);
    return n;
}
static LI_PYROWAVE_REASSEMBLY_RESULT send(MLPyrowaveReassembly *c, uint32_t id,
    uint16_t count, uint16_t block, uint16_t index, uint8_t value, uint64_t now)
{
    uint8_t packet[65536]; size_t n = wire(packet, id, count, block, index, value, false);
    return MLPyrowaveReassemblyPush(c, packet, n, now);
}
static void inactive(MLPyrowaveReassembly *c, uint32_t id, size_t capacity)
{
    CHECK(!c->active && c->saved == NULL && c->used == 0 && c->lastId == id);
    CHECK(c->capacity == capacity && (c->storage != NULL) == (capacity != 0));
    CHECK(c->common.blocks == NULL && c->common.received == NULL);
    CHECK(c->commonBudget == 0 && c->savedBytes == 0 && c->parityBytes == 0);
    CHECK(c->firstUs == 0 && c->deadlineUs == 0);
}
static void bounds(MLPyrowaveReassembly *c, size_t packets)
{
    size_t table = ((size_t)c->descriptor.blockCount + 1) * sizeof(SavedPacket);
    CHECK(c->active && c->saved == (SavedPacket *)c->storage);
    CHECK(c->used == table + packets * (64 + c->descriptor.fecBlockPayloadSize));
    CHECK(c->used <= c->capacity && c->capacity + ALLOC_OVERHEAD <= SAVED_LIMIT);
    CHECK(fitsTotal(c->capacity + ALLOC_OVERHEAD, c->commonBudget));
    for (uint32_t i = 0; i <= c->descriptor.blockCount; ++i) if (c->saved[i].bytes) {
        CHECK(c->saved[i].length == 64 + c->descriptor.fecBlockPayloadSize);
        CHECK(c->saved[i].bytes >= c->storage + table);
        CHECK(c->saved[i].bytes <= c->storage + c->used);
        CHECK(c->saved[i].length <= (size_t)(c->storage + c->used - c->saved[i].bytes));
    }
}
static void copy(MLPyrowaveReassembly *c, uint8_t a, uint8_t b, size_t n, uint64_t now)
{
    uint8_t output[64]; size_t length = 0; MLPyrowaveFrameInfo info;
    uint32_t id = c->lastId; size_t cap = c->capacity; uint8_t *storage = c->storage;
    CHECK(MLPyrowaveReassemblyCopyFrame(c, now, output, 64, &length, &info) == R(COMPLETE));
    CHECK(length == n && info.frameId == id && info.rtpTimestamp == 900);
    for (size_t i = 0; i < n; ++i) CHECK(output[i] == (i < 8 ? a : b));
    inactive(c, id, cap); CHECK(c->storage == storage);
}
int main(void)
{
    failCalloc = true; CHECK(MLPyrowaveReassemblyCreate() == NULL);
    CHECK(live == 0 && liveCount == 0 && callocs == 1 && mallocs == 0);
    MLPyrowaveReassembly *c = MLPyrowaveReassemblyCreate(); CHECK(c != NULL);
    uint8_t packet[65536]; size_t n = wire(packet, 1, 2, 8, 0, 1, false);
    CHECK(MLPyrowaveReassemblyPush(c, packet, n, 0) == R(ACCEPTED));
    size_t cap = 3 * (sizeof(SavedPacket) + 72); uint8_t *storage = c->storage;
    CHECK(mallocs == 1 && callocs == 2 && c->capacity == cap); bounds(c, 1);
    CHECK(c->saved[0].bytes != packet && memcmp(c->saved[0].bytes, packet, n) == 0);
    packet[n - 1] ^= 1; CHECK(c->saved[0].bytes[n - 1] == 1); packet[n - 1] ^= 1;
    CHECK(MLPyrowaveReassemblyPush(c, packet, n, 1) == R(DUPLICATE)); bounds(c, 1);
    CHECK(send(c, 1, 2, 8, 1, 2, 2) == R(COMPLETE)); bounds(c, 2);
    n = wire(packet, 1, 2, 8, 0, 9, true);
    CHECK(MLPyrowaveReassemblyPush(c, packet, n, 3) == R(ACCEPTED)); bounds(c, 3);
    CHECK(c->used == c->capacity && mallocs == 1); copy(c, 1, 2, 16, 4);
    CHECK(send(c, 2, 2, 8, 1, 4, 10) == R(ACCEPTED));
    CHECK(c->storage == storage && c->saved[0].bytes == NULL && c->saved[2].bytes == NULL);
    CHECK(send(c, 2, 2, 8, 0, 3, 11) == R(COMPLETE)); bounds(c, 2); copy(c, 3, 4, 16, 12);
    CHECK(send(c, 3, 1, 8, 0, 5, 20) == R(COMPLETE)); bounds(c, 1); copy(c, 5, 0, 8, 21);
    CHECK(c->storage == storage && c->capacity == cap && mallocs == 1);
    CHECK(send(c, 4, 2, 8, 0, 6, 30) == R(ACCEPTED));
    size_t f = frees; expectEntry = sizeof(*c); failMalloc = true; peak = live;
    CHECK(send(c, 5, 4, 16, 0, 7, 31) == R(OUT_OF_MEMORY));
    inactive(c, 5, 0); CHECK(frees == f + 1 && entryLive == sizeof(*c));
    CHECK(live == sizeof(*c) && liveCount == 1 && peak == sizeof(*c) + cap);
    CHECK(send(c, 5, 4, 16, 0, 7, 32) == R(STALE) && mallocs == 2);
    CHECK(send(c, 6, 2, 8, 0, 8, 33) == R(ACCEPTED)); bounds(c, 1);
    CHECK(mallocs == 3 && entryLive == sizeof(*c));
    f = frees; peak = live;
    CHECK(send(c, 7, 4, 16, 0, 9, 34) == R(ACCEPTED)); bounds(c, 1);
    cap = 5 * (sizeof(SavedPacket) + 80); storage = c->storage;
    CHECK(c->capacity == cap && mallocs == 4 && frees == f + 1);
    CHECK(entryLive == sizeof(*c) && peak == sizeof(*c) + cap);
    expectEntry = SIZE_MAX;
    CHECK(send(c, 8, 1, 8, 0, 10, 35) == R(COMPLETE)); copy(c, 10, 0, 8, 35);
    CHECK(c->storage == storage && mallocs == 4);
    CHECK(send(c, 9, 1, 8, 0, 11, 36) == R(COMPLETE));
    CHECK(send(c, 9, 1, 8, 0, 12, 37) == R(INVALID_PACKET)); inactive(c, 9, cap);
    CHECK(send(c, 9, 1, 8, 0, 11, 38) == R(STALE));
    CHECK(send(c, 10, 1, 8, 0, 12, 39) == R(COMPLETE)); copy(c, 12, 0, 8, 39);
    CHECK(send(c, 11, 2, 8, 0, 13, 40) == R(ACCEPTED));
    MLPyrowaveReassemblyDiscardFrame(c); inactive(c, 11, cap);
    CHECK(send(c, 11, 2, 8, 0, 13, 41) == R(STALE));
    CHECK(send(c, 12, 2, 8, 0, 14, 42) == R(ACCEPTED));
    CHECK(send(c, 12, 2, 8, 0, 14, 100042) == R(EXPIRED)); inactive(c, 12, cap);
    CHECK(send(c, 12, 2, 8, 0, 14, 100043) == R(STALE));
    CHECK(send(c, 13, 2, 8, 0, 15, 100044) == R(ACCEPTED));
    CHECK(send(c, 14, 513, 32768, 0, 0, 100045) == R(OVERSIZE)); inactive(c, 14, cap);
    CHECK(c->storage == storage && mallocs == 4);
    CHECK(send(c, 14, 513, 32768, 0, 0, 100046) == R(STALE));
    CHECK(send(c, 15, 1, 8, 0, 16, 100047) == R(COMPLETE)); copy(c, 16, 0, 8, 100047);
    CHECK(send(c, 16, 2, 8, 0, 17, 100048) == R(ACCEPTED));
    c->used = c->capacity + 1; /* Guard subtraction before appending. */
    CHECK(send(c, 16, 2, 8, 1, 18, 100049) == R(OVERSIZE)); inactive(c, 16, cap);
    CHECK(send(c, 17, 2, 8, 0, 17, 100050) == R(ACCEPTED)); c->used = c->capacity;
    CHECK(send(c, 17, 2, 8, 1, 18, 100051) == R(OVERSIZE)); inactive(c, 17, cap);
    CHECK(c->storage == storage && mallocs == 4 && callocs == 2);
    f = frees; MLPyrowaveReassemblyDestroy(c);
    CHECK(frees == f + 2 && live == 0 && liveCount == 0);
    for (size_t i = 0; i < 8; ++i) CHECK(owned[i].pointer == NULL);
    puts("PyroWave native duplicate arena checks passed (not receive-path allocation totals)");
    return EXIT_SUCCESS;
}
