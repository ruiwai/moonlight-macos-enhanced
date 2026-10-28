// SPDX-License-Identifier: GPL-3.0-only
#ifndef ML_PYROWAVE_METAL_PAYLOAD_H
#define ML_PYROWAVE_METAL_PAYLOAD_H

#import <Metal/Metal.h>
#import "MLPyrowaveCoordinator.h"
#include "MLPyrowaveReceiver.hpp"
#include <cstdint>
#include <limits>
#include <unistd.h>

// Internal Objective-C++ only. The resource owns the immutable frame and its pool;
// the normal command buffer retains the bound resource through its last reader.
// A lease is never reused while Metal holds the alias, not merely until a completion
// callback runs. No decoder/UI retain cycle; vector destruction returns the lease.
// Never write CPU bytes after validation; default/non-pooled vectors cannot alias.
static inline id<MTLBuffer> MLPyrowaveAliasPayload(id<MTLDevice> device, MLPyrowaveFrame *frame)
{
    if (!device || !frame) return nil;
    if (@available(macOS 13.0, *)) {
        if (!device.hasUnifiedMemory) return nil;
        const auto *received = [frame validatedFrame];
        if (!received || !received->codec) return nil;
        const auto &f = *received->codec;
        const auto &bytes = f.bytes();
        if (!bytes.get_allocator().pool || !bytes.data() || bytes.size() < 24 ||
            f.payloadSize() > 16u * 1024u * 1024u || bytes.size() != f.payloadSize() + 16)
            return nil;
        const long systemPageSize = sysconf(_SC_PAGESIZE);
        if (systemPageSize <= 0) return nil;
        const size_t pageSize = static_cast<size_t>(systemPageSize);
        // Pool alignment is fixed at 64 KiB; check actual runtime page alignment.
        if (reinterpret_cast<uintptr_t>(bytes.data()) % pageSize ||
            bytes.size() > std::numeric_limits<size_t>::max() - (pageSize - 1)) return nil;
        const size_t length = ((bytes.size() + pageSize - 1) / pageSize) * pageSize;
        if (length > MLPyrowave::PayloadPool::SlotBytes || length > device.maxBufferLength ||
            length > std::numeric_limits<NSUInteger>::max()) return nil;
        MLPyrowaveFrame * __strong owner = frame;
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:const_cast<uint8_t *>(bytes.data())
            length:length options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked
            deallocator:^(void *, NSUInteger) { (void)owner; }]; // Never free the pointer here.
        if (!buffer || buffer.device != device || buffer.storageMode != MTLStorageModeShared ||
            buffer.length != length || buffer.contents != bytes.data()) return nil;
        return buffer;
    }
    return nil;
}
#endif
