//
//  HIDSupport_Internal.h
//  Moonlight for macOS
//
//  Created by Michael Kenny on 26/12/17.
//  Copyright © 2017 Moonlight Stream. All rights reserved.
//
#import "HIDSupport.h"
#import "Controller.h"
#import "Ticks.h"
#import "HIDSupportRumbleTypes.h"
#import "MLPrecisionTouchpad.h"

#import "MoonlightEnhanced-Swift.h"
#include <limits.h>
#include <math.h>
#include "HIDMouseWheelPolicy.h"

#include "Limelight.h"
#include "Limelight-internal.h"

#import <Carbon/Carbon.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hid/IOHIDElement.h>

@import GameController;

@interface HIDSupport () <CoreHIDMouseDriverDelegate>
@property (nonatomic) unsigned short authoredRumbleLow;
@property (nonatomic) unsigned short authoredRumbleHigh;
@property (nonatomic) uint32_t authoredRumbleSequence;
@property (nonatomic) BOOL hasAuthoredRumbleSequence;
@property (nonatomic) BOOL authoredRumbleDeliveryScheduled;
@property (nonatomic) NSTimeInterval authoredRumbleUpdatedAt;
@property (nonatomic, strong) NSTimer *authoredRumbleTimer;
@property(nonatomic) uint32_t controllerShortcutPhysicalButtons;
@property (nonatomic) dispatch_queue_t rumbleQueue;
@property (nonatomic, strong) NSDictionary *mappings;
@property (nonatomic) IOHIDManagerRef hidManager;
@property (nonatomic, strong) Controller *controller;
@property (nonatomic) BOOL gamepadMouseModeEnabled;
@property (nonatomic) BOOL gamepadMouseModeLongPressMenuEnabled;
@property (nonatomic) BOOL gamepadMenuPressed;
@property (nonatomic, strong) NSTimer *gamepadMenuTimer;
@property (nonatomic) CVDisplayLinkRef displayLink;
@property (nonatomic) UInt8 previousLowFreqMotor;
@property (nonatomic) UInt8 previousHighFreqMotor;
@property (atomic) UInt16 nextLowFreqMotor;
@property (atomic) UInt16 nextHighFreqMotor;
@property (atomic) dispatch_semaphore_t rumbleSemaphore;
@property (atomic) BOOL closeRumble;
@property (atomic) BOOL isRumbleTimer;
@property (nonatomic) PS4StatePacket_t lastPS4State;
@property (nonatomic) PS5StatePacket_t lastPS5State;
@property (nonatomic) NSInteger controllerDriver;
@property (nonatomic) BOOL isPS5Bluetooth;
@property (nonatomic) uint8_t ds5OutputSequence;

@property (nonatomic) SwitchSimpleStatePacket_t lastSimpleSwitchState;
@property (nonatomic) SwitchStatePacket_t lastSwitchState;

@property (atomic) dispatch_semaphore_t hidReadSemaphore;
@property (atomic) BOOL vibrationEnableResponded;
@property (atomic) BOOL waitingForVibrationEnable;
@property (atomic) UInt32 startedWaitingForVibrationEnable;
@property (nonatomic) dispatch_queue_t enableVibrationQueue;

@property (nonatomic) BOOL switchUsingBluetooth;
@property (nonatomic) UInt8 switchCommandNumber;
@property (nonatomic) BOOL switchRumbleActive;
@property (nonatomic) UInt32 switchUnRumbleSent;
@property (nonatomic) BOOL switchRumblePending;
@property (nonatomic) BOOL switchRumbleZeroPending;
@property (nonatomic) UInt32 switchUnRumblePending;

@property (nonatomic, strong) Ticks *ticks;

@property (nonatomic) id mouseConnectObserver;
@property (nonatomic) id mouseDisconnectObserver;

@property (nonatomic) BOOL useGCMouse;
@property (nonatomic) BOOL useCoreHIDMouse;
@property (nonatomic, strong) CoreHIDMouseDriver *coreHIDMouseDriver;
@property (nonatomic) BOOL coreHIDMouseDidDeliverMovement;
@property (nonatomic) BOOL coreHIDMouseRuntimeFailed;
@property (nonatomic) NSUInteger keyboardPhysicalModifierSourceMask;
@property (nonatomic) NSUInteger keyboardRemoteModifierMask;
@property (nonatomic) NSUInteger keyboardDeferredShortcutTranslationCommandMask;
@property (atomic) BOOL coreHIDFreeMouseAbsoluteSyncScheduled;
@property (atomic) uint64_t coreHIDFreeMouseAbsoluteSyncToken;
@property (nonatomic) dispatch_queue_t inputQueue;
@property (nonatomic, strong) MLPrecisionTouchpad *precisionTouchpad;
@property (atomic) uint64_t suppressRelativeMouseUntilMs;
@property (nonatomic, strong) NSObject *freeMouseVirtualCursorLock;
@property (atomic) BOOL freeMouseVirtualCursorRequestedActive;
@property (nonatomic) BOOL freeMouseVirtualCursorHasAnchor;
@property (nonatomic) NSPoint freeMouseVirtualCursorPoint;
@property (nonatomic) NSPoint freeMouseVirtualCursorLastAnchorPoint;
@property (nonatomic) NSSize freeMouseVirtualCursorReferenceSize;
@property (nonatomic) double freeMouseVirtualCursorGainX;
@property (nonatomic) double freeMouseVirtualCursorGainY;
@property (nonatomic) double freeMouseVirtualCursorRawSinceAnchorX;
@property (nonatomic) double freeMouseVirtualCursorRawSinceAnchorY;
@property (nonatomic, strong) NSObject *inputDiagnosticsLock;
@property (atomic) BOOL inputDiagnosticsEnabled;
@property (nonatomic) NSUInteger inputDiagnosticsDetailedLogSequence;
@property (nonatomic) NSUInteger inputDiagnosticsRemainingDetailedLogs;
@property (nonatomic) NSUInteger inputDiagnosticsRemainingScrollDetailedLogs;
@property (nonatomic) uint64_t scrollTraceSequence;
@property (nonatomic) uint64_t activeScrollTraceId;
@property (nonatomic) uint64_t activeScrollTraceStartedMs;
@property (nonatomic) uint64_t activeScrollTraceLastEventMs;
@property (nonatomic, copy) NSString *activeScrollTraceSource;
@property (nonatomic) NSUInteger inputDiagnosticsMouseMoveEvents;
@property (nonatomic) NSUInteger inputDiagnosticsNonZeroRelativeEvents;
@property (nonatomic) NSUInteger inputDiagnosticsRelativeDispatches;
@property (nonatomic) NSUInteger inputDiagnosticsAbsoluteDispatches;
@property (nonatomic) NSUInteger inputDiagnosticsAbsoluteDuplicateSkips;
@property (nonatomic) NSUInteger inputDiagnosticsCoreHIDRawEvents;
@property (nonatomic) NSUInteger inputDiagnosticsCoreHIDDispatches;
@property (nonatomic) NSUInteger inputDiagnosticsSuppressedRelativeEvents;
@property (nonatomic) NSInteger inputDiagnosticsRawRelativeDeltaX;
@property (nonatomic) NSInteger inputDiagnosticsRawRelativeDeltaY;
@property (nonatomic) NSInteger inputDiagnosticsSentRelativeDeltaX;
@property (nonatomic) NSInteger inputDiagnosticsSentRelativeDeltaY;
@property (nonatomic) short lastAbsolutePointerHostX;
@property (nonatomic) short lastAbsolutePointerHostY;
@property (nonatomic) short lastAbsolutePointerReferenceWidth;
@property (nonatomic) short lastAbsolutePointerReferenceHeight;
@property (nonatomic) uint64_t lastAbsolutePointerAtMs;
@property (nonatomic, copy) NSString *lastAbsolutePointerSource;
@property (nonatomic) uint32_t pressedMouseButtonsMask;
// SDL-style residuals and the observed wheel epoch are inputQueue-owned.
@property (nonatomic) HIDWheelState wheelState;
@property (nonatomic) uint64_t wheelQueueEpoch;

- (void)sendControllerEvent;
- (void)updateGamepadMenuGesture;
- (void)setGamepadMouseModeActive:(BOOL)active;
- (KeyboardCompatibilityMode)keyboardCompatibilityMode;
- (BOOL)usesKeyboardCommandToControlCompatibility;
- (BOOL)usesKeyboardLeftControlWinSwapCompatibility;
- (BOOL)usesKeyboardOptionCommandSwapCompatibility;
- (BOOL)usesKeyboardShortcutTranslationCompatibility;
- (void)updateKeyboardPhysicalModifierStateFromEvent:(NSEvent *)event;
- (BOOL)shouldApplyKeyboardShortcutTranslationForEvent:(NSEvent *)event;
- (NSUInteger)desiredRemoteKeyboardModifierMaskForEvent:(NSEvent *)event;
- (void)syncKeyboardModifierStateForEvent:(NSEvent *)event;
- (char)translatedModifierFlagsForEvent:(NSEvent *)event;
- (short)translateKeyCodeWithEvent:(NSEvent *)event;
- (char)translateKeyModifierWithEvent:(NSEvent *)event;
- (void)handleDpad:(NSInteger)intValue;
- (void)updateButtonFlags:(int)flag state:(BOOL)set;
- (void)setupHidManager;
- (void)tearDownHidManagerOnMainThread;
- (BOOL)reserveDetailedInputDiagnosticsLogSequence:(NSUInteger *)sequence;
- (void)syncScrollTraceDiagnosticsPreferenceToInputContext;
- (uint64_t)prepareScrollTraceFromSource:(NSString *)source
                               rawDeltaX:(CGFloat)rawDeltaX
                               rawDeltaY:(CGFloat)rawDeltaY
                                   phase:(NSEventPhase)phase
                           momentumPhase:(NSEventPhase)momentumPhase
                        hasPreciseDeltas:(BOOL)hasPreciseDeltas;
- (void)recordRelativeInputDiagnosticsFrom:(NSString *)source
                                 rawDeltaX:(CGFloat)rawDeltaX
                                 rawDeltaY:(CGFloat)rawDeltaY
                                sentDeltaX:(short)sentDeltaX
                                sentDeltaY:(short)sentDeltaY
                                suppressed:(BOOL)suppressed;
- (void)recordAbsoluteInputDiagnosticsFrom:(NSString *)source
                                         x:(short)x
                                         y:(short)y
                                     width:(short)width
                                    height:(short)height;
- (void)resetRelativeMouseMotion;
- (uint64_t)inputMotionEpoch;
- (uint64_t)inputWheelEpoch;
- (void)resetMouseWheel;
- (void)resetPointerInputLifecycle;
- (void)dispatchRelativeMouseDeltaX:(CGFloat)deltaX
                             deltaY:(CGFloat)deltaY
                          sourceTag:(NSString *)sourceTag;
- (void)setupCoreHIDMouseDriverIfNeeded;
- (void)tearDownCoreHIDMouseDriver;
- (void)refreshMouseInputConfiguration;
- (void)recordMouseButtonDiagnosticsAction:(NSString *)action
                                    button:(int)button
                                      mask:(uint32_t)mask
                                 synthetic:(BOOL)synthetic;
- (void)recordScrollInputDiagnosticsMode:(NSString *)mode
                                 traceId:(uint64_t)traceId
                               rawDeltaX:(CGFloat)rawDeltaX
                               rawDeltaY:(CGFloat)rawDeltaY
                            rawWheelDeltaX:(NSInteger)rawWheelDeltaX
                            rawWheelDeltaY:(NSInteger)rawWheelDeltaY
                        normalizedDeltaX:(CGFloat)normalizedDeltaX
                        normalizedDeltaY:(CGFloat)normalizedDeltaY
                              continuous:(BOOL)continuous
                        hasPreciseDeltas:(BOOL)hasPreciseDeltas
                             lineDeltaX:(NSInteger)lineDeltaX
                             lineDeltaY:(NSInteger)lineDeltaY
                            pointDeltaX:(NSInteger)pointDeltaX
                            pointDeltaY:(NSInteger)pointDeltaY
                          fixedDeltaXRaw:(NSInteger)fixedDeltaXRaw
                          fixedDeltaYRaw:(NSInteger)fixedDeltaYRaw
                                   phase:(NSEventPhase)phase
                           momentumPhase:(NSEventPhase)momentumPhase
                              dispatchedX:(short)dispatchedX
                              dispatchedY:(short)dispatchedY;
@end

@interface HIDSupport (PointerInternal)
- (void)registerMouseCallbacks:(GCMouse *)mouse API_AVAILABLE(macos(11.0));
- (void)unregisterMouseCallbacks:(GCMouse *)mouse API_AVAILABLE(macos(11.0));
- (BOOL)initializeDisplayLink;
- (BOOL)dispatchVirtualFreeMouseDeltaX:(double)deltaX
                                deltaY:(double)deltaY
                             sourceTag:(NSString *)sourceTag;
- (BOOL)getFreeMouseVirtualCursorPoint:(NSPoint *)viewPoint
                         referenceSize:(NSSize *)referenceSize;
@end

@interface HIDSupport (RumbleInternal)
- (int)hidGetFeatureReport:(IOHIDDeviceRef)device data:(unsigned char *)data length:(size_t)length;
- (void)rumbleSync;
- (void)runRumbleLoop;
- (IOHIDDeviceRef)getFirstDevice;
- (int)switch_RumbleJoystick:(IOHIDDeviceRef)device lowFreqMotor:(UInt16)lowFreqMotor highFreqMotor:(UInt16)highFreqMotor;
- (BOOL)setVibrationEnabled:(UInt8)enabled;
- (BOOL)writeSubcommand:(ESwitchSubcommandIDs)ucCommandID
                   pBuf:(UInt8 *)pBuf
                  ucLen:(UInt8)ucLen
                ppReply:(SwitchSubcommandInputPacket_t **)ppReply;
- (void)constructSubcommand:(ESwitchSubcommandIDs)ucCommandID
                       pBuf:(UInt8 *)pBuf
                      ucLen:(UInt8)ucLen
                  outPacket:(SwitchSubcommandOutputPacket_t *)outPacket;
- (int)switchSendPendingRumble:(IOHIDDeviceRef)device;
- (int)switchActuallyRumbleJoystick:(IOHIDDeviceRef)device low_frequency_rumble:(UInt16)low_frequency_rumble high_frequency_rumble:(UInt16)high_frequency_rumble;
- (void)setNeutralRumble:(SwitchRumbleData_t *)pRumble;
- (void)switchEncodeRumble:(SwitchRumbleData_t *)pRumble
                usHighFreq:(UInt16)usHighFreq
            ucHighFreqAmp:(UInt8)ucHighFreqAmp
                 ucLowFreq:(UInt8)ucLowFreq
              usLowFreqAmp:(UInt16)usLowFreqAmp;
- (BOOL)writeRumble:(IOHIDDeviceRef)device;
- (BOOL)writePacket:(IOHIDDeviceRef)device pBuf:(void *)pBuf ucLen:(UInt8)ucLen;
@end

static inline UInt16 usbIdFromDevice(IOHIDDeviceRef device, NSString *key) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, (__bridge CFStringRef)key);
    if (value == NULL) {
        return 0;
    }
    return [(__bridge NSNumber *)value unsignedShortValue];
}

static inline BOOL isNintendo(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x057E && (productId == 0x2009);
}

static inline BOOL isXbox(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x045E && (productId == 0x02FD || productId == 0x0B13);
}

static inline BOOL isKingKong(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x045E && productId == 0x02e0;
}

static inline BOOL isPlayStation(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x054C && (productId == 0x09CC || productId == 0x05C4 || productId == 0x0CE6);
}

static inline BOOL isPS4(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x054C && (productId == 0x09CC || productId == 0x05c4);
}

static inline BOOL isPS5(IOHIDDeviceRef device) {
    UInt16 vendorId = usbIdFromDevice(device, @kIOHIDVendorIDKey);
    UInt16 productId = usbIdFromDevice(device, @kIOHIDProductIDKey);
    return vendorId == 0x054C && (productId == 0x0ce6);
}

static inline PML_INPUT_STREAM_CONTEXT HIDInputContext(HIDSupport *support) {
    PML_INPUT_STREAM_CONTEXT ctx = (PML_INPUT_STREAM_CONTEXT)support.inputContext;
    if (ctx != NULL && ctx->connectionContext != NULL) {
        LiSetThreadConnectionContext(ctx->connectionContext);
    }
    return ctx;
}

static inline bool HIDValidateInputContext(PML_INPUT_STREAM_CONTEXT ctx, const char *op) {
    static CFAbsoluteTime lastLogTime = 0;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (ctx == NULL) {
        if (now - lastLogTime > 1.0) {
            Log(LOG_W, @"Input dropped (%s): inputContext is NULL", op);
            lastLogTime = now;
        }
        return false;
    }
    if (!LiInputContextIsInitialized(ctx)) {
        if (now - lastLogTime > 1.0) {
            Log(LOG_W, @"Input dropped (%s): inputContext not initialized (ctx=%p conn=%p)", op, ctx, LiInputContextGetConnectionCtx(ctx));
            lastLogTime = now;
        }
        return false;
    }
    return true;
}

static inline void HIDDispatchInput(HIDSupport *support, PML_INPUT_STREAM_CONTEXT inputCtx, dispatch_block_t block) {
    if (inputCtx == NULL) {
        return;
    }
    PML_CONNECTION_CONTEXT connCtx = inputCtx->connectionContext;
    dispatch_async(support.inputQueue, ^{
        if (connCtx != NULL) {
            LiSetThreadConnectionContext(connCtx);
        }
        block();
    });
}

static inline CGFloat HIDPointerSensitivityForHost(TemporaryHost *host) {
    CGFloat sensitivity = [SettingsClass pointerSensitivityFor:host.uuid];
    if (!isfinite(sensitivity) || sensitivity <= 0.0) {
        return 1.0;
    }
    return MIN(MAX(sensitivity, 0.25), 3.0);
}

static inline CGFloat HIDAbsoluteMouseReferencePrecisionScale(NSSize referenceSize) {
    CGFloat width = MAX(referenceSize.width, 1.0);
    CGFloat height = MAX(referenceSize.height, 1.0);
    CGFloat maxDimension = ceil(MAX(width, height));
    if (!isfinite(maxDimension) || maxDimension <= 0.0) {
        return 1.0;
    }

    CGFloat maxSafeScale = floor((((CGFloat)SHRT_MAX) - 1.0) / maxDimension);
    if (!isfinite(maxSafeScale) || maxSafeScale < 1.0) {
        return 1.0;
    }

    return MIN(8.0, maxSafeScale);
}

static inline BOOL HIDAbsoluteMousePositionForViewPoint(NSPoint viewPoint,
                                                        NSSize referenceSize,
                                                        BOOL clampToBounds,
                                                        short *hostX,
                                                        short *hostY,
                                                        short *referenceWidth,
                                                        short *referenceHeight) {
    CGFloat width = MAX(referenceSize.width, 1.0);
    CGFloat height = MAX(referenceSize.height, 1.0);
    CGFloat precisionScale = HIDAbsoluteMouseReferencePrecisionScale(referenceSize);
    CGFloat scaledWidth = MAX(1.0, floor(width * precisionScale));
    CGFloat scaledHeight = MAX(1.0, floor(height * precisionScale));
    CGFloat x = isfinite(viewPoint.x) ? viewPoint.x : 0.0;
    CGFloat y = isfinite(viewPoint.y) ? viewPoint.y : 0.0;

    if (clampToBounds) {
        x = MIN(MAX(x, 0.0), width - 1.0);
        y = MIN(MAX(y, 0.0), height - 1.0);
    }

    if (hostX != NULL) {
        *hostX = (short)lrint(MIN(MAX(x * precisionScale, 0.0), scaledWidth - 1.0));
    }
    if (hostY != NULL) {
        *hostY = (short)lrint(MIN(MAX((height - y) * precisionScale, 0.0), scaledHeight));
    }
    if (referenceWidth != NULL) {
        *referenceWidth = (short)lrint(scaledWidth);
    }
    if (referenceHeight != NULL) {
        *referenceHeight = (short)lrint(scaledHeight);
    }

    return YES;
}

static inline BOOL HIDAbsoluteMouseReferenceForEvent(NSEvent *event,
                                                     NSPoint *viewPoint,
                                                     NSSize *referenceSize) {
    if (event == nil || event.window == nil || event.window.contentView == nil) {
        return NO;
    }

    NSView *contentView = event.window.contentView;
    NSPoint convertedPoint = [contentView convertPoint:event.locationInWindow fromView:nil];
    NSSize boundsSize = contentView.bounds.size;
    if (!isfinite(convertedPoint.x) || !isfinite(convertedPoint.y) ||
        !isfinite(boundsSize.width) || !isfinite(boundsSize.height)) {
        return NO;
    }

    if (viewPoint != NULL) {
        *viewPoint = convertedPoint;
    }
    if (referenceSize != NULL) {
        *referenceSize = boundsSize;
    }

    return YES;
}

static inline BOOL HIDAbsoluteMouseReferenceForCurrentPointer(NSWindow *window,
                                                              NSPoint *viewPoint,
                                                              NSSize *referenceSize) {
    if (window == nil || window.contentView == nil) {
        return NO;
    }

    NSView *contentView = window.contentView;
    NSPoint screenPoint = [NSEvent mouseLocation];
    NSPoint windowPoint = [window convertPointFromScreen:screenPoint];
    NSPoint convertedPoint = [contentView convertPoint:windowPoint fromView:nil];
    NSSize boundsSize = contentView.bounds.size;
    if (!isfinite(convertedPoint.x) || !isfinite(convertedPoint.y) ||
        !isfinite(boundsSize.width) || !isfinite(boundsSize.height)) {
        return NO;
    }

    if (viewPoint != NULL) {
        *viewPoint = convertedPoint;
    }
    if (referenceSize != NULL) {
        *referenceSize = boundsSize;
    }

    return YES;
}

static inline BOOL HIDShouldUseHybridFreeMouseMotion(HIDSupport *support) {
    return [SettingsClass shouldUseHybridFreeMouseMotionFor:support.host.uuid];
}

static inline BOOL HIDShouldUseCoreHIDFreeMouseAbsoluteSync(HIDSupport *support) {
    NSString *mouseMode = [SettingsClass mouseModeFor:support.host.uuid];
    BOOL remoteDesktopMode = [mouseMode isEqualToString:@"remote"];
    return remoteDesktopMode &&
           support.useCoreHIDMouse &&
           !support.useGCMouse &&
           !support.coreHIDMouseRuntimeFailed &&
           HIDShouldUseHybridFreeMouseMotion(support);
}

static inline BOOL HIDShouldUseAbsolutePointerPath(HIDSupport *support, NSInteger touchscreenMode) {
    NSString *mouseMode = [SettingsClass mouseModeFor:support.host.uuid];
    BOOL remoteDesktopMode = [mouseMode isEqualToString:@"remote"];
    return (remoteDesktopMode &&
            (!HIDShouldUseHybridFreeMouseMotion(support) || support.coreHIDMouseRuntimeFailed))
           || touchscreenMode == 1;
}

static inline BOOL HIDShouldSuppressRelativeMouse(HIDSupport *support) {
    uint64_t untilMs = support.suppressRelativeMouseUntilMs;
    return untilMs > 0 && LiGetMillis() < untilMs;
}

static inline void HIDInvalidateCoreHIDFreeMouseAbsoluteSync(HIDSupport *support) {
    support.coreHIDFreeMouseAbsoluteSyncScheduled = NO;
    support.coreHIDFreeMouseAbsoluteSyncToken += 1;
}

static inline uint32_t HIDMouseButtonBitForButton(int button) {
    switch (button) {
        case BUTTON_LEFT:
            return 1u << 0;
        case BUTTON_MIDDLE:
            return 1u << 1;
        case BUTTON_RIGHT:
            return 1u << 2;
        case BUTTON_X1:
            return 1u << 3;
        case BUTTON_X2:
            return 1u << 4;
        default:
            return 0;
    }
}
