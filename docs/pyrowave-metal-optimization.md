# Native PyroWave Metal optimization v1

Scope: the native `Limelight/Stream/PyroWave` path, not MoltenVK or an assertion
that all optimizations are done. The following changes exist in current source;
static inspection is not runtime acceptance. No benchmarking hardware is available
locally; no benchmark gain, steady-state speedup or future CI pass is claimed.
Local golden/Metal builds and execution remain prohibited by the
[test policy](../tests/pyrowave/README.md). Cache/shader changes have static review
only so far; standalone draw-gate C11/C++14 checks passed with ASan/UBSan.

## Implemented changes and limits

- Receiver recovery copies into owned payload+16-byte-guard scratch once.
  Session `CodecValidator` caches immutable layout masks; `ValidateOwned` fully
  validates before moving scratch into the immutable frame, avoiding a second
  payload copy. The borrowed-input convenience wrapper still copies on success.
  Cached masks, offsets and vector capacities stay budgeted; OR/popcount counts
  pooled significant/sign bits without changing duplicate/validation semantics.
- Dequant batches independent bands per component/level using grid.z=3 (levels
  0..3) or 4 (level 4): 42 dispatches become 13. IDWT remains 13 dispatches with
  cached pipeline bindings/shared push bytes once per level. FP32 and barriers
  remain; fewer CPU encode calls are not proof of lower GPU time.
- Receive parsing and main-thread drawable acquisition/encoding run outside the
  app `_decodeSubmitLock`; short state snapshots and epoch/peer checks guard
  results. Coordinator receive/stop serialization remains. A try-take collision
  returns `ReceiveBusy`, distinct from GPU Busy; draw gating waits for receive
  completion, using the receive serial to avoid a lost wake if it already ended.
- Draw notifications, GPU completion and presented callbacks weakly capture app/
  renderer owners, with epoch checks. GPU retirement retains private resources
  through the command buffer's last reader, not a CPU frame or UI owner.
- Opt-in signposts expose CPU stages and command-buffer completion (below).
- `kernelsWithDevice:error:` serializes under
  `@synchronized([MLPyrowaveMetalKernels class])`. One strong process-lifetime
  static retains only the most-recent successful immutable pipeline/sampler bundle,
  keyed by exact device object identity, not registryID. A hit returns the same
  object and clears error; nil always rejects. Misses build privately/uncached and
  publish only on success; any failure preserves the last good entry. Eviction
  does not invalidate bundles callers retain. No decoder, texture, heap or frame
  is cached. This removes repeated preflight/setup compilation, not measured
  steady-state speedup. Runtime source compilation and fastMath=NO remain.

## Profiling contract

Use Instruments Points of Interest with subsystem `com.moonlight.pyrowave`,
category `pipeline`. Exact signpost names and boundaries:

| Name | Interpretation |
| --- | --- |
| `Receive` | Synchronous peer receive/reassembly/validation, then app lock reacquisition, stats and scheduling tail; not pure parser time or initial app-lock wait. Fields: frameId, epoch, size; end result. |
| `Drawable` | `currentDrawable` acquisition only; valid flag. Tagged by epoch, **not frameId**; do not join it to a frame by invented identity. |
| `Upload` | CPU payload memcpy/guard initialization and byte-to-word offset conversion; frameId/generation. Not a GPU transfer-duration measurement. |
| `Encode` | CPU compute-command encoding (dequant + IDWT) only, not upload, render encoding, commit or GPU execution; frameId/generation, success. |
| `Submitted` | Event immediately before commit after final deadline check; frameId/generation. Not presentation acknowledgment. |
| `GPUComplete` | Completion callback event; frameId/generation/success. gpuSeconds appears only for valid GPU timestamps. GPU end minus start covers the entire command buffer, including render, not decode alone. |

Metal labels: command buffer `PyroWave decode and present`; compute encoders
`PyroWave dequant` and `PyroWave IDWT`. Use GPU trace/counters for pass attribution.
The callback event's CPU timestamp is not the GPU end timestamp. GPU end/completion
and drawable `presentedTime` are separate observations; only presented callbacks
acknowledge display. HUD decode time is submission-to-completion wall time, not
GPU time. Per-packet Receive tracing can perturb timing; compare trace-off runs.

## Reproducible baseline before further optimization

Compare revisions on the same Mac/GPU, OS/build settings, workload and exact
input/encoder version, quality, resolution, color/range/siting and source cadence.
Record pipeline setup cold/warm separately from steady state; warm up compilation,
resource reuse and clocks. Record thermal/power state and avoid throttling bias.
Keep duration/sample count and drop/admission policies matched; report p50/p95/p99
for receive, upload, CPU encoding, GPU total and end-to-present latency alongside
throughput, dropped/busy/expired counts and peak/steady memory. Do not hide drops
by reporting only surviving-frame latency. Preserve source timestamps/cadence;
host/RTP/local clocks need an explicit validated mapping before subtraction.
Capture GPU traces/counters for dequant, IDWT and render, and compare trace-on/off
overhead. Archive settings, revisions and raw distributions, not just averages.
No measurements were collected here.

## Acceptance matrix (required, not assumed passed)

| Gate | Evidence boundary |
| --- | --- |
| Existing CI | All existing Common protocol/reassembly/negotiation, metadata, codec/packetizer, receiver/composition/coordinator, stats/draw-gate and Metal kernel/decoder/renderer checks remain required, plus the app build. Fatal sanitizer/strict-warning policy stays. |
| Kernel cache assertions | CI-only same-device object identity/error clearing, nil rejection after hit and cache survival. Unsupported GPU exit 77 is a skip, never a pass. No current execution claim. |
| Batch equivalence | Synthetic per-band vs batched dequant bytewise comparisons include nonzero/sparse records, 3/4 bands and edge geometry; same shader on both sides is not independent numerical proof. |
| Numerical/pixel acceptance | Existing sparse/zero dispatch smoke and batching checks are **not** a full nonzero IDWT oracle. Full real-encoder nonzero corpus with independent reference planes, edges/aprons, quantization, chroma and agreed numeric bounds is prerequisite for FP16, fusion or hand-tuned SIMD. |
| Lifetime/live/performance | Metal validation, stop/resize/control races, final-reader lifetime, live encrypted transport/color/presentation and matched hardware profiling remain separate gates; CI smoke does not prove them. |

## Deferred / not implemented

- Persistent upload arena plus direct reassembly ownership: preserve identical/
  conflicting duplicate semantics, retirement/deadlines and peak-capacity budgets;
  the current shared GPU upload buffers still receive a CPU payload copy.
- Multi-slot GPU overlap: define bounded last-reader leases for upload, pyramid
  and output resources; measure latency/backpressure, not just throughput.
- FP16 storage or arithmetic: require numeric goldens/error bounds from the full
  nonzero corpus; no precision or fast-math change is included.
- IDWT hand-tuned SIMD or fusion: prove apron/mirror ordering, quantization and
  chroma behavior against independent references before profiling acceptance.
- Precompiled metallib: future packaging/version/device validation work; the
  one-entry in-process cache is not a precompiled or disk library cache.
