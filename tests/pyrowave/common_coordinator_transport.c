// Test scaffolding only. All transport dispatch/ownership is actual Common.
#include "Limelight-internal.h"
#include "common_coordinator_transport.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
// This integration target always runs under ASan; inspect shadow, never bytes
// from the freed allocation, to prove Common consumed its ownership on return.
#include <sanitizer/asan_interface.h>
#undef VideoCallbacks
#undef NegotiatedVideoFormat

struct CommonTransport { ML_CONNECTION_CONTEXT connection; };

CommonTransport *transportCreate(const LI_PYROWAVE_PACKET_CALLBACKS *callbacks)
{
    CommonTransport *t = calloc(1, sizeof(*t));
    assert(t != NULL && callbacks != NULL);
    ML_CONNECTION_CONTEXT *c = &t->connection;
    LiInitializeVideoCallbacks(&c->VideoCallbacks);
    c->NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    c->NegotiatedPyrowave.maxPacketSize = 256;
    c->VideoCallbacks.capabilities = CAPABILITY_PYROWAVE;
    c->VideoCallbacks.pyrowave = *callbacks;
    c->videoContext.connectionContext = c;
    c->videoContext.depacketizerContext.connectionContext = c;
    RtpvInitializeQueue(&c->videoContext.rtpQueue, &c->videoContext.depacketizerContext);
    return t;
}

void transportTls(CommonTransport *t)
{
    LiSetThreadConnectionContext(t ? &t->connection : NULL);
}

void transportDestroy(CommonTransport *t)
{
    if (!t) return;
    transportTls(NULL);
    RtpvCleanupQueue(&t->connection.videoContext.rtpQueue);
    free(t);
}

void transportGates(CommonTransport *t, unsigned version, bool capable)
{
    t->connection.VideoCallbacks.pyrowave.version = version;
    t->connection.VideoCallbacks.capabilities = capable ? CAPABILITY_PYROWAVE : 0;
}

bool transportFullFrame(const CommonTransport *t)
{
    return t->connection.videoContext.receivedFullFrame;
}

void transportResetObservation(CommonTransport *t)
{
    // Test observation only, not production state management or a fake ack.
    t->connection.videoContext.receivedFullFrame = false;
}

static PRTP_PACKET envelope(const uint8_t *bytes, size_t length, uint32_t id,
                           uint32_t timestamp, int *total)
{
    assert(bytes != NULL && length <= 256);
    const size_t offset = sizeof(RTP_PACKET) + 4;
    *total = (int)(offset + sizeof(NV_VIDEO_PACKET) + length);
    PRTP_PACKET packet = malloc((size_t)*total);
    assert(packet != NULL);
    memset(packet, 0, (size_t)*total);
    packet->header = 0x90;
    packet->sequenceNumber = 60000; // Inner PYRF owns order, not legacy RTP FEC.
    packet->timestamp = timestamp;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)((uint8_t *)packet + offset);
    nv->frameIndex = id;
    nv->flags = FLAG_SOF | FLAG_EOF;
    memcpy(nv + 1, bytes, length);
    return packet;
}

void transportDirect(CommonTransport *t, const uint8_t *bytes, size_t length,
                     uint32_t id, uint32_t timestamp, uint64_t receive, uint64_t presentation)
{
    RTPV_QUEUE_ENTRY entry = {0};
    entry.packet = envelope(bytes, length, id, timestamp, &entry.length);
    // Direct depacketizer input, including NV frameIndex, is already host endian.
    entry.rtpTimestamp = timestamp;
    entry.receiveTimeUs = receive;
    entry.presentationTimeUs = presentation;
    queueRtpPacketCtx(&t->connection.videoContext.depacketizerContext, &entry);
    // entry.packet is freed by Common, even when its callback gate rejects.
    assert(__asan_address_is_poisoned(entry.packet));
}

uint64_t transportRtp(CommonTransport *t, const uint8_t *bytes, size_t length,
                      uint32_t id, uint32_t timestamp)
{
    RTPV_QUEUE_ENTRY entry = {0};
    int total;
    PRTP_PACKET packet = envelope(bytes, length, LE32(id), timestamp, &total);
    // RtpvAddPacket consumes host-endian RTP and converts little-endian NV.
    int result = RtpvAddPacket(&t->connection.videoContext.rtpQueue, packet, total, &entry);
    if (result == RTPF_RET_REJECTED) {
        free(packet); // Rejection alone retains caller ownership.
        return 0;
    }
    assert(result == RTPF_RET_QUEUED);
    assert(__asan_address_is_poisoned(packet));
    return entry.receiveTimeUs; // No dereference of the now-freed packet.
}
