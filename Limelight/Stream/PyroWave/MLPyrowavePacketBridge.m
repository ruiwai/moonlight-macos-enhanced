// SPDX-License-Identifier: GPL-3.0-only
#import "MLPyrowavePacketBridge.h"

int MLPyrowaveOfferedFormats(int preference, BOOL preflightReady, int legacyFormats)
{
    if (preference != 3) return legacyFormats;
    return VIDEO_FORMAT_H264 | (preflightReady ? VIDEO_FORMAT_PYROWAVE : 0);
}

BOOL MLPyrowaveValidatePresentationConfiguration(MLPyrowaveConfiguration c, int frameRate)
{
    return c.generation != 0 && c.maxPacketSize >= 129 && c.maxPacketSize <= 65536 &&
        c.width >= 2 && c.width <= 16384 && !(c.width & 1) &&
        c.height >= 2 && c.height <= 16384 && !(c.height & 1) &&
        c.range <= 1 && c.siting <= 1 && frameRate > 0;
}

static int MLPyrowaveSubmit(void *context, const LI_PYROWAVE_RECEIVED_PACKET *packet)
{
    if (!context || !packet) return LI_PYROWAVE_PACKET_IGNORED;
    return [(__bridge id<MLPyrowavePacketSink>)context submitPyrowavePacket:packet];
}

LI_PYROWAVE_PACKET_CALLBACKS MLPyrowaveCallbacksForSink(id<MLPyrowavePacketSink> sink)
{
    if (!sink) return (LI_PYROWAVE_PACKET_CALLBACKS){0};
    return (LI_PYROWAVE_PACKET_CALLBACKS){
        LI_PYROWAVE_PACKET_CALLBACK_VERSION2, (__bridge void *)sink, MLPyrowaveSubmit};
}
