// Test-only C shield: no Common internal headers enter Objective-C++.
#ifndef COMMON_COORDINATOR_TRANSPORT_H
#define COMMON_COORDINATOR_TRANSPORT_H
#include "Limelight.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CommonTransport CommonTransport;
CommonTransport *transportCreate(const LI_PYROWAVE_PACKET_CALLBACKS *callbacks);
void transportDestroy(CommonTransport *transport);
void transportTls(CommonTransport *transport);
void transportGates(CommonTransport *transport, unsigned version, bool capable);
// Observation only: reset clears the watchdog latch to detect a fresh ack.
// Neither function synthesizes a successful callback or sets the latch true.
bool transportFullFrame(const CommonTransport *transport);
void transportResetObservation(CommonTransport *transport);
// Copies inner bytes into an owned RTP/NV allocation; real Common frees it
// synchronously before return. Direct input uses deterministic explicit times.
void transportDirect(CommonTransport *transport, const uint8_t *bytes, size_t length,
                     uint32_t id, uint32_t timestamp, uint64_t receive, uint64_t presentation);
// Actual queue ingress, host-endian RTP / little-endian NV as Common expects.
// Returns receive time (LiGetMicroseconds epoch), or zero on rejection, where
// only caller-owned storage is freed by this helper. No sockets/startup.
uint64_t transportRtp(CommonTransport *transport, const uint8_t *bytes, size_t length,
                      uint32_t id, uint32_t timestamp);
#ifdef __cplusplus
}
#endif
#endif
