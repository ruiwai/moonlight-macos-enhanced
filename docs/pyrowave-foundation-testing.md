# PyroWave experimental Foundation testing

## Reference and prerequisites

- Reference the repository-local vanilla `foundation-sunshine/` checkout
  at `441b2b88051a00c91d562b24ac703adefc4cf87f`.
  Do not substitute `~/foundation-sunshine` or an older `foundation-sunshine186…` fork.
- The deployed Windows host must be built with PyroWave and have a working
  Vulkan runtime. Local source presence does not establish the deployed build,
  capture setup or runtime availability; these remain unknown. No host changes
  are made by this client change.
- Use supported Apple Silicon/macOS hardware for the client's experimental Metal
  decoder/presenter. Pipeline/resource preflight is not a GPU correctness test.
- Host SDP must advertise all five `x-ss-pyrowave.*` attributes: protocolVersion,
  bitstreamVersion, payloadVersion, capabilityFlags and maxPacketSize.
  Existing native Common negotiates protocol/bitstream/payload **2/2/3** and emits
  `x-ml-pyrowave.*` on the selected path. Required SDR/range/reassembly/deadline/
  block-FEC/metadata flags and viable inner packet size **>128, <=65536** apply.
  There is no new HTTP codec parameter or guessed header-based capability.

## Start small and observe the actual selection

1. Test SDR **1920x1080**, **2560x1440** and **3840x2160**; 1280x720 is an optional
   baseline, not a forced size. The drawable canvas is aspect-preserving and
   bounded by both source and backing dimensions; Retina/4K backing alone no
   longer charges a full-4K canvas. The compositor scales the canvas to the window.
   Preflight/setup account for two maximum source-size canvases within the hard
   **512 MiB** session cap, not a full-cap allocation; actual charges must fit. Geometry is
   not memory proof. Resizing neither recreates the decoder nor raises the budget.
2. In Video settings choose **PyroWave (Experimental)** (stored index 3).
   Indices 0 H.264, 1 H.265 and 2 AV1, and default H.264, remain unchanged;
   there is no codec Auto option and nothing implicitly opts into PyroWave.
3. Saved HDR, 10-bit SDR, YUV 4:4:4 and renderer preferences are not rewritten.
   Effective experimental configuration is SDR BT.709, 8-bit 4:2:0 with the
   dedicated Metal path (currently centered siting). An unavailable path offers
   only H.264. Fallback uses normal legacy setup and the saved renderer choice.
4. Inspect client logs for **PyroWave offered**, or
   **PyroWave unavailable; offering H.264 fallback** with the exact localized
   preflight error. Offering is not selection: inspect **Actual negotiated video
   format** for **PyroWave selected** or **initial fallback**, and the diagnostics
   overlay's actual codec (**PyroWave** versus the negotiated legacy codec).
   A capable client can still get initial H.264 when the host lacks the contract.
   The saved preference is not rewritten and there is no midstream codec switch.
5. Record client/host revisions, deployed host build/runtime and capture details,
   resolution/FPS, source/backing/drawable setup log, negotiated codec and exact
   offer/fallback/setup failure reasons (do not infer a budget cause from fallback).
   Report visual corruption, color errors, stalls, memory issues and stop/restart
   behavior separately. Do not infer PyroWave performance from H.264 fallback:
   the ordinary legacy risk assessment is deliberately skipped for index 3.

## Evidence boundary

The vanilla host supports VRR, cursor and HDR features; this client slice is
SDR-only. Do not infer old-fork restrictions or promise those host features have
been validated through this client path. Capture/deployment remain user-specific.

Native compilation/tests of this snapshot await macOS Actions. Local static
checks and synthetic policy/transport tests cannot establish real encoder pixel
accuracy, Metal API/shader safety, GPU lifetime correctness or interoperability.
Hardware smoke tests returning 77 are **skipped**, not passed. GPU correctness
verification and real Foundation streaming tests remain deferred: no proven
interoperability or “works now” claim is made until matching hardware evidence.
