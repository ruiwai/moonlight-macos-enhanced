# PyroWave native integration plan — contract v1

## Status, pins and scope

Common raw bridge version 2 at `9af0c8c` retains SDR negotiation introduced at `e26d480`; app selection/preflight, coordinator, Metal and presentation remain planned/unwired.
Parent app baseline: `6e97b2feac3e56b296299c1006018a69bbb713e2`; Native Common repin: `9af0c8cc9be21afce5c735c7876ba03884f7d84c` (new-pin parent CI pending).
Common remains the public `https://github.com/ruiwai/moonlight-common-c.git`; do not swap in Foundation Common (different streaming API/ABI).
Wire contract: protocol **2**, bitstream **2**, payload **3**; exact encoder reference: `cc91cbf6363721d19d135d05ca8e3d8665ea6afd`.
Initial scope: supported Apple Silicon, SDR BT.709, 8-bit 4:2:0, negotiated range/siting; FP32 arithmetic **and storage**. HDR/444/10-bit and FP16 are deferred.
Local designs `docs/pyrowave-protocol-contract.md` and `docs/pyrowave-metal-decoder.md` provide background, not tracked remote documents or current dependency/build inventory.
Their older missing-dependency statements do not describe this baseline; neither design is changed here.
Existing symbols below are source anchors. **Common raw descriptor/route/gates and negotiation are implemented; new app APIs, selection/preflight and coordinator/renderer wiring below are PROPOSED.**

## Current → planned call path and selection

`Native/` means this checkout; `Common/` means `moonlight-common/moonlight-common-c/`.

| Existing file:symbol | Current responsibility → planned app/integration work |
| --- | --- |
| `Native/Limelight/Stream/Connection.m:DrDecoderSetup`, `_drCallbacks`, `RegisterConnection` | Legacy renderer setup and per-connection callbacks/context → own preflight/selection and the per-connection raw-packet descriptor. |
| `Common/src/RtspConnection.c:performRtspHandshakeCtx` | Implemented bounded five-attribute PyroWave parser/helper and per-context retained contract → app opt-in/preflight remains planned. |
| `Common/src/SdpGenerator.c:getAttributesList` | Implemented selected SDR contract validation and ANNOUNCE feature/attributes; unselected legacy output remains unchanged. |
| `Common/src/RtpVideoQueue.c:RtpvAddPacket` | Implemented selected PyroWave version-2 route bypasses legacy FEC, contiguous RTP sequence and 16-bit frame-index rejection; preserves full outer identity/timing → app reassembly ownership remains planned. |
| `Common/src/VideoDepacketizer.c:queueRtpPacketCtx` | Implemented selected PyroWave raw hook before millisecond conversion/legacy H.26x processing; validates bounds, borrows synchronously and frees → app callback remains planned. |
| `Native/Limelight/Stream/Connection.m:DrSubmitDecodeUnit`, `DrStop` | Legacy submit/stop → separate raw packet dispatch and generation-safe coordinator teardown; preserve legacy behavior. |
| `Native/Limelight/Stream/VideoDecoderRenderer.m:setupMetalRenderer`, `encodePresentFromTexture:toDrawable:commandBuffer:` | Existing renderer device/scheduling/RGB presentation → new three-plane decode/color bridge, not a VideoToolbox compressed-format alias. |

Implemented Common negotiation at `e26d480` parses all five complete decimal attributes under `x-ss-pyrowave.*`:
`protocolVersion`, `bitstreamVersion`, `payloadVersion`, `capabilityFlags`, `maxPacketSize`.
Bounds input lengths and arithmetic; rejects missing, malformed, duplicate/conflicting attributes, signs, suffixes and overflow before uint16/uint32 narrowing/allocation.
Requires local **and** peer 2/2/3, negotiated packet budget within Common's 65..65536 inner-packet bounds and viable outer RTP/NV budget (inner >128).
Requires REASSEMBLY, FRAME_DEADLINE, BLOCK_AWARE_FEC, FRAME_METADATA, selected SDR709/420 mode and selected limited/full-range capabilities; retains the result per context.
ANNOUNCE emits the five `x-ml-pyrowave.*` fields and feature flag only for that negotiated capable path, never for unselected/fallback sessions.
The raw callback path enforces negotiated inner-packet size. PROPOSED app validation must match frame color/depth/sampling/siting to the session; no unsupported modes or partial frames.
PROPOSED app selection: only explicit PyroWave preference may offer the PyroWave/H.264 fallback mask, after actual-device + dimensions + compiled-pipeline/full-footprint preflight.
Auto and legacy masks remain unchanged. If preflight fails, disclose unavailability/initial H.264 fallback; never advertise an incapable PyroWave path.
Expose capability to `Limelight/macOS/ViewControllers/SettingsVideoPane.swift` and `Connection.m` only after full preflight.
Propagate an explicit new preference through `SettingsModel+DerivedValues.swift:videoCodecs`, `SettingsModel+Persistence.swift`, `SettingsObjCBridge.swift`,
and `StreamViewController.m` (currently clamps codec indices to 0..2) before `Connection.m` constructs masks.
Preserve all existing stored indices; handle the new value explicitly rather than extending Connection's legacy ordinal comparisons.
Display actual negotiated format and initial fallback reason without rewriting saved preference; no in-stream codec switch.

## Raw Common implemented / app ownership PROPOSED

Implemented `Common/src/Limelight.h:DECODER_RENDERER_CALLBACKS.pyrowave` is a same-build/source extension, not old-binary ABI compatible:
`LI_PYROWAVE_PACKET_CALLBACKS` contains version, application context and submit pointer; public `DECODE_UNIT` layout is unchanged by this bridge.
Version 2 borrows immutable inner packet bytes/length, full outer frame ID and RTP timestamp, and `receiveTimeUs` / `presentationTimeUs` scalars; version 1 is rejected.
`submit` returns `int`: only `LI_PYROWAVE_PACKET_VALIDATED_FRAME` after synchronous owned complete PYRF recovery, protected TLV/codec validation and local-deadline validation satisfies the first-full-frame watchdog. Partial/malformed/expired/replayed input returns `LI_PYROWAVE_PACKET_IGNORED`; ignored/unknown results do not acknowledge a frame. This is not GPU/presentation success.
Submission is synchronous; retain nothing borrowed. Common owns/frees the RTP allocation once after return, also on raw-path drops; callbacks must not free it or synchronously stop/destroy Common.
`queueRtpPacketCtx` checks runtime RTP/extension/NV lengths and raw bounds before dispatch, before microsecond truncation or `processRtpPayload`.
No callback when unselected; `VideoStream.c:startVideoStreamCtx` already rejects selected PyroWave without capability, submit or descriptor version 2 before setup/sockets.
The raw route returns without legacy decode-unit/IDR processing; unselected legacy ownership remains unchanged. No app calls this bridge, registers a callback or opts in.
PROPOSED app ownership below is not implemented; app selection/preflight, coordinator, Metal and presentation remain disabled/unwired despite Common negotiation support.
Context remains valid until the receive thread joins. `VideoStream.c:stopVideoStreamCtx` calls stop **before** joins, cleanup **after** joins:
submissions may follow stop and be discarded, but never follow cleanup. A callback extension must preserve this existing contract.
`Connection.m` passes itself through application context backed by the owned `RegisterConnection` registry lifetime through `LiStopConnectionCtx`/joins.
The setup `void *` / `(__bridge void *)self` does **not** retain Connection; registry and explicit strong shutdown ownership do.
The raw callback snapshots strong coordinator and renderer references under `_stateLock`; do not depend solely on thread-local `CurrentConnection`.
Stop invalidates admission/generation under the same state discipline **before** `ConnectionClearRuntimeTargets`; cleanup releases context after joins.
Snapshots protect lifetime, not admission: publication must serialize with stop and recheck generation so a late snapshot cannot publish stale work.

`Common/src/VideoStream.c` now bypasses the encrypted pre-auth frame filter for PyroWave: authenticate first, then require app full-ID/replay validation; AES-GCM and timeout behavior are unchanged.
Watchdog result routing and the filter helper are implemented and synthetically tested at `9af0c8c`, not live encrypted/network validation.
Actual callback → owned recovery/TLV/codec/deadline validator composition and live encrypted reorder/wrap/replay/watchdog coverage remain required before enabling.

## PROPOSED coordinator: synchronous CPU boundary, bounded GPU admission

Add an ObjC++ coordinator inside `Limelight/Stream/PyroWave`; its Objective-C boundary shields existing `.m` callers from C++.
Reuse existing `MLPyrowaveReassemblyPush` / `MLPyrowaveReassemblyCopyFrame` (`MLPyrowaveReassembly.h`), including protected metadata validation via
`MLPyrowaveValidateMetadata` (`MLPyrowaveMetadata.h`), then `MLPyrowave::ValidateCodecFrame` → owned immutable `ValidatedCodecFrame` (`MLPyrowaveCodec.hpp`).
Serialize reassembly/Copy/validation/admission; all borrowed packet bytes must be consumed or copied before the synchronous callback returns.
Reconcile outer frame identity with the parsed PYRF identity; validate repeated descriptors within each domain before owned frame publication.
Do **not** require outer RTP and inner PYRF timestamp equality: the pinned host derives inner timestamps from frame number/FPS,
but outer RTP timestamps from capture/pacing relative to `video_epoch`. Preserve both snapshots with the owned frame.
Use local monotonic time for deadlines, outer presentation timing for existing scheduling, and inner timestamps for PYRF descriptor consistency.
Cross-domain subtraction or conversion needs separately verified mapping; no such mapping or cross-domain equality is established here.
Preserve full inner identity, metadata, receive/deadline and presentation timing through ownership and rendering; neither codec short sequence nor milliseconds replaces them.
Keep RTP, host and local monotonic clock domains explicit; no subtraction/arithmetic between unrelated clocks without a validated mapping.
Only complete, valid, same-session frames may enter **one bounded waiting-frame** slot before Metal encoding. Incomplete/malformed/conflicting frames drop.
Check `deadline <= now` at Copy/admission and before GPU publication; duplicates cannot extend deadlines. Ordinary loss/late/busy drops cause no IDR or decoder recreation.
Stop/reconfigure generations serialize with admission and completion publication; no stale callback may refill a stopped/new session's waiting slot or renderer.

## PROPOSED Metal and rendering contract

Inject the actual renderer device and queue: current symbols are `VideoDecoderRenderer.m:_device` and `_commandQueue`, **not** `_metalDevice`.
Do not create a generic default device/queue. Preflight the injected device, requested layout, texture usages and all compiled pipelines.
Compile the exact pinned dequant + FP32 IDWT kernels with notices preserved; do not feed untrusted input through the unsafe upstream Metal parser/API.
Use validated offsets only: they are **BYTE offsets** into the full original frame. Divide valid offsets by four only for an unchanged whole-frame upload.
Preserve `UINT32_MAX` absent entries and the 16 initialized zero guard bytes; validate alignment/read bounds, payload base and sparse/zero-band initialization.
Use at most **four leases + one waiting frame**, **256 MiB total session-owned CPU/GPU allocation accounting**, including reconfiguration overlap.
Charge the reassembly wrapper's conservative **128 MiB upper bound**, codec validation peak **32 MiB**, waiting ownership, upload/offset buffers, intermediates and outputs;
avoid double-counting shared allocations but never omit simultaneous peaks, alignment/guards or retirement overlap. Protected frame <=16 MiB; metadata <=65535 bytes.
Checked exact-footprint preflight may reject otherwise library-valid even dimensions (2..16384); do not promise all library-valid sizes fit.
Use completion-driven nonblocking acquisition/drop, never slot `waitUntilCompleted` or unbounded allocation. A shared pyramid is safe only with serialized reuse on one ordered queue.
Retain uploads until last GPU read and output leases until the **last rendering/compositor GPU reader**, not merely decode completion.
Abandoning uncommitted work releases unpublished leases; command errors retire safely and never publish stale/uninitialized output.
Stop detaches presentation/drops waiting work, then asynchronously retires in-flight generations via lifetime-safe state, not destroyed UI/session pointers.
Reconfiguration preflights old/new overlap or drains safely; no live resource mutation/free to satisfy a timeout or memory cap.
Add a separate three-plane **R8Unorm Y/Cb/Cr** renderer input (Y W×H; Cb/Cr W/2×H/2), with negotiated BT.709 range and chroma siting.
Implement a new three-plane color conversion, not the current two-plane NV12 kernel. Apply range/transfer exactly once, then reuse existing scheduling and RGB present helper.

## Build membership: reconcile inventory with evidence

`Moonlight.xcodeproj/project.pbxproj` lacks target `fileSystemSynchronizedGroups` and explicit renderer Sources membership; navigator/exception entries alone prove neither inclusion nor exclusion.
Nevertheless `/tmp/cs-kz-b8779e4-build-reviewed.log:8898` records `CompileC ... VideoDecoderRenderer.m` for macOS at b8779e4.
Do **not** claim the renderer is uncompiled or add a duplicate renderer source. The exact checked-in membership versus effective-build mechanism remains to be reconciled.
That reviewed log has no compile records for the existing three `MLPyrowaveMetadata.c`, `MLPyrowaveReassembly.c`, `MLPyrowaveCodec.cpp` translation units.
Common `PyrowaveProtocol.c` + `PyrowaveNegotiation.c` explicit file references → build files → src group → Sources entries already exist; baseline `6e97b2f` app CI compiled each exactly once. This repin needs no PBX change; new-pin app CI remains pending.
Common `PyrowaveReassembly.c` (not called by negotiation), native validators and coordinator sources remain future membership work; Apple-only tests stay out of the app target. Headers are not Sources.
Verify actual single compilation/link ownership in macOS CI, including synchronized/generated effects; no blanket membership migration.

## Remaining implementation and acceptance evidence

| Gate / owner | Required evidence, not supplied by architecture |
| --- | --- |
| RTSP/SDP + Connection | Common helper/serializer synthetic cases passed on macOS with fatal ASan/UBSan (run 40400754728); app preflight/selection/fallback reporting and live RTSP remain future gates. |
| Actual RTP → raw callback | Common bridge test covers short RTP/NV/extensions, nonzero sequence, full outer frame-ID reorder/wrap, exact identity/microseconds, gates and context isolation, plus v2 watchdog result routing/filter helper. Synthetic arbitrary payload only: no actual owned PYRF/TLV/codec/deadline validator composition. Still require that composition and live encrypted/network reorder/wrap/replay/watchdog, inner/outer identity conflicts, ownership and loss/deadline/block-FEC coverage end-to-end. |
| Existing CPU validator composition | Extend `tests/pyrowave/test_metadata.c`, `test_reassembly.c`, `test_codec.cpp`, `test_pipeline.cpp` as appropriate; preserve owned bytes/offsets/metadata/identity, poison/reset and bounded mutation coverage. |
| Codec → GPU differential | Exact pinned **GPU encoder fixtures** and agreed per-plane error bounds for FP32, sparse/zero bands, edges and dimensions. Current `test_packetizer.cpp` uses synthetic packetizer inputs, **not GPU encoder fixtures**. |
| Metal + lifecycle | API/shader validation; busy pool, cancel/uncommitted/error, last-reader lifetime, stop/late callbacks/reconfigure, memory peaks and generation isolation. |
| Renderer color | Limited/full-range and center/left-siting reference patches through final BT.709 display conversion, not only luma checksums. |
| App / release / host | macOS app build with unique Sources records, draft artifact evidence and user evidence from a real Foundation host/device stream before any functionality claim. |

Public Common [run 40390473131](https://github.com/ruiwai/moonlight-common-c/actions/runs/40390473131) at `65f43ca` passed **one** `pyrowave-packet-callback-test` CTest on `macos-15`, ASan/UBSan fatal, after initial truncated-fixture UB was fixed.
`Common/tests/PyrowavePacketCallbackTest.c` links the actual Common library/queue/depacketizer and verifies precise outer metadata with synthetic arbitrary bytes, not real codec payload or validator composition.
Runtime test registration is macOS-only after the explicit `if(APPLE)` guard change at `6e4befc`; broader Common CI there compiled Linux/macOS, but Windows still failed C11 atomics/`ssize_t`. No universal green claim.
Historical parent `a8a647d` CI with Common `65f43ca` was verified all green (PyroWave, Input11: 11/11 CTests, macOS build/draft); this is not new-pin evidence.
At Common `e26d480`, [run 40400754728](https://github.com/ruiwai/moonlight-common-c/actions/runs/40400754728) passed two actual-Common callback + negotiation CTests on `macos-15`, ASan/UBSan fatal. Historical `5303fde`: attempt 1 had no runner; attempt 2 callback passed, negotiation fixture lacked `VideoPortNumber`; `e26d480` fixes only that fixture.
`Common/tests/PyrowaveNegotiationTest.c` tests actual Common helpers/serializer with synthetic inputs, not network RTSP.
At exact Common `9af0c8cc9be21afce5c735c7876ba03884f7d84c`, [run 40403395078](https://github.com/ruiwai/moonlight-common-c/actions/runs/40403395078) passed both callback + negotiation CTests with fatal ASan/UBSan (`/tmp/pyrowave-9af0c8c-reviewed.log:249-258`).
Parent baseline `6e97b2f` passed all three CI workflows: PyroWave CPU prerequisites, Input (12/12 CTests), macOS app build/draft; reviewed logs `/tmp/pyrowave-6e97b2f-404017*-reviewed.log` include single compilation of both Common units. This is baseline evidence, not new-pin parent CI, which remains pending.
These establish their recorded CPU/build/draft boundaries only: no GPU correctness, live-stream interoperability or performance claim.
Future implementation proceeds in focused bounded changes: one focused commit/push at a time, inspect `gh` run/job logs before the next; acceptance evidence must match the final snapshot.
The earlier membership/documentation-only slice is historical; this repin uses local static inspection only. App/runtime gates and new-pin parent CI remain open; no app callback/coordinator, Metal, preflight or selection is enabled.
