# Native PyroWave Metal optimization v1

Scope: the native `Limelight/Stream/PyroWave` path, not MoltenVK or an assertion
that all optimizations are done. The following changes exist in current source;
static inspection is not runtime acceptance. No benchmarking hardware is available
locally; no benchmark gain, steady-state speedup or future CI pass is claimed.
Local golden/Metal builds and execution remain prohibited by the
[test policy](../tests/pyrowave/README.md). Cache/shader changes have static review
only so far; standalone draw-gate C11/C++14 checks passed with ASan/UBSan.

## Implemented changes and limits

- CPU duplicate-packet arena /v1 reuses one bounded allocation for the native
  saved-packet table and wire bytes, removing that allocation churn after warmup.
  Full original bytes are still copied and exact duplicates still use memcmp.
  Full retained high-water capacity is charged to the cap and freed on destroy;
  growth frees before allocating, with no overlap. Common allocations remain.
  CI-only whitebox counting/context+arena fault injection is pending execution;
  Common allocation fault paths are not injected. This is not zero-copy or a
  speedup claim: reassembly copy remains, with shared-upload-copy fallback.
  Pooled payload aliasing below changes ownership, not precision.
- Receiver recovery copies into owned payload+16-byte-guard scratch once.
  `CodecBytes`, a C++14 optionally pooled overwrite byte vector, removes
  payload preclear; complete exact-length copy initializes payload before validation.
  Only the 16 guards are zero-initialized before publication, not the payload.
  Session `CodecValidator` caches immutable layout masks; `ValidateOwned` fully
  validates before moving scratch into the immutable frame, avoiding a second
  payload copy. The borrowed-input wrapper validates first, then copies payload
  into overwrite storage and zeros its 16 guards before publication.
  Cached masks, offsets and vector capacities stay budgeted; OR/popcount counts
  pooled significant/sign bits without changing duplicate/validation semantics.
- Receiver-only payload leases use exactly three lazy 64-KiB-aligned fixed slots,
  each 16 MiB+64 KiB (maximum 48 MiB+192 KiB). Exhaustion drops/retires the frame;
  release permits strictly newer IDs, not replay. Stop/destruction preserves held
  frames through allocator shared ownership; payload is never precleared.
  Codec accounting charges a whole slot, not logical vector capacity. Metal reserves
  all three, even inactive, in addition to 128 MiB reassembly +32 MiB validator
  +1 MiB bookkeeping +2*(Payload+offsetBytes+4096). Double counting default CPU
  inputs/leases is deliberately conservative; the 512 MiB cap is unchanged.
  Standalone pool local sanitizer tests are allowed; golden/codec/receiver/Metal
  execution stays CI-only. No performance gain is claimed.
- On unified-memory devices, validated pooled payloads use a per-submit no-copy
  shared/tracked Metal buffer after runtime page-alignment/rounded-length checks.
  Its deallocator block retains the immutable frame/pool, never frees the pointer;
  normal command-buffer resource retention prevents lease reuse through last read.
  No alias is cached on decoder/pool; no CPU writes follow validation. Nil alias,
  including default/non-pooled vectors, uses the unchanged fixed upload fallback.
  Offsets still memcpy; all three slots and fallback heaps remain budgeted together.
  Alias tests now blit payload+16 guards, drop CPU owners before commit, then check 10s completion/readback/retirement.
  Weak-owner/slot-exhaustion/recycling assertions and pooled GPU smoke are NOT EXECUTED; macOS lifetime/output remain unverified.
- The record scanner writes one native GPU-ready uint32_t `wordOffsets()` table
  (4-byte words; UINT32_MAX absent) directly while its cursor stays in bytes.
  Duplicate checks convert prior word entries to bytes for read32/exact memcmp.
  Metal validates the exact layout count and byte bounds, then memcpy's the table;
  no second table or post-validation conversion pass. Wire, shaders, precision and
  ownership are unchanged. Changed codec/caller tests are NOT EXECUTED, awaiting CI;
  portable draw-gate/stats checks do not prove codec/GPU correctness or speedup.
- Dequant batches independent bands per component/level using grid.z=3 (levels
  0..3) or 4 (level 4): 42 dispatches become 13. IDWT remains 13 dispatches with
  cached pipeline bindings/shared push bytes once per level. FP32 and barriers
  remain; dequant now uses a concurrent encoder, pending GPU/benchmark acceptance.
  Fewer CPU encode calls are not proof of lower GPU time.
- Receive parsing and main-thread drawable acquisition/encoding run outside the
  app `_decodeSubmitLock`; short state snapshots and epoch/peer checks guard
  results. Coordinator receive/stop serialization remains. A try-take collision
  returns `ReceiveBusy`, distinct from GPU Busy; draw gating waits for receive
  completion, using the receive serial to avoid a lost wake if it already ended.
- Draw notifications, GPU completion and presented callbacks weakly capture app/
  renderer owners, with epoch checks. GPU retirement retains private resources
  through the command buffer's last reader. Aliased resources also retain their
  immutable CPU frame/pool; completion itself does not capture a frame/UI owner.
- Opt-in signposts expose CPU stages and command-buffer completion (below).
- `kernelsWithDevice:error:` serializes under
  `@synchronized([MLPyrowaveMetalKernels class])`. One strong process-lifetime
  static retains only the most-recent successful immutable pipeline/sampler bundle,
  keyed by exact device object identity, not registryID. A hit returns the same
  object and clears error; nil always rejects. Misses build privately/uncached and
  publish only on success; any failure preserves the last good entry. Eviction
  does not invalidate bundles callers retain. No decoder, texture, heap or frame
  is cached. This removes repeated preflight/setup compilation, not measured
  steady-state speedup. Runtime source fallback and fastMath=NO remain.
- Offline compute packaging uses `scripts/build_pyrowave_metallibs.py`: exact
  raw strings compile separately as Metal 3.0, macOS 13 minimum, no fast math.
  Both links must succeed before staged publication of `pyrowave-idwt.metallib`,
  `pyrowave-dequant.metallib` and their same-stem `.sha256` companions under
  bundle `Resources/PyroWave`. Intermediates live in temporary work directories.
  SHA256 covers UTF-8 `pyrowave-metal3.0-fp32-v1\n` plus exact source bytes.
  Only a matching signature permits bundle URL loading; missing/stale/unloadable
  libraries fall back to existing source options, without external lookup or a
  persistent cache. Libraries are labeled `PyroWave bundled` / `PyroWave runtime`.
  The macOS target packages before helper phases; its deployment floor stays 12,
  while PyroWave preflight/offline Metal require 13. Renderer shaders are unchanged.
  Mocked Python tests/static review do not verify Apple tool flag support, app
  build/signing, library loading or GPU output; these remain CI/hardware gates.
  Missing resources, stale fingerprints and matching-fingerprint/unloadable libraries
  now HAVE TEST SOURCE in fresh temporary bundles; NOT EXECUTED (CI/hardware only).
  Decoder/renderer CI runs both bare runtime and copied bundled-resource apps;
  these paths are NOT EXECUTED here, not evidence of Apple loading or GPU correctness.
  No binary artifacts, precision changes or measured speedup are included.

## Remaining-work status (original proposals)

Current acceptance is static/portable only; no measured speedup or all-complete claim.
“Done” below means source implemented, not GPU/performance acceptance.

| Proposal | Status / remaining evidence or architecture |
| --- | --- |
| GPU-ready offsets | Done `26cbcea`; codec/GPU CI remains required. |
| Bounded payload pool | Done `66ae7e4`; portable allocator checks only locally. |
| Direct Metal alias | Done `f434bbe`; eliminates eligible payload upload copy, but Common → pool `CopyFrame` remains; lifetime/hardware gates pending. |
| Precompiled compute | Done `33f1bef`; Apple build/load and fallback CI source NOT EXECUTED. |
| FP16 storage/arithmetic | BLOCKED: independent real-encoder nonzero golden/reference planes, numeric bounds and hardware acceptance. |
| Fusion | BLOCKED: apron/order/reference proof and profiling evidence. |
| Direct packet/FEC in-place reassembly | BLOCKED: Common API ownership redesign; aliasing does not solve it. |
| Extra GPU slots | Deferred pending latency/backpressure benchmarks and bounded lifetime/budget design. |

Blockers are evidence/architecture requirements, not user approval; no approval is needed.

## Profiling contract

Use Instruments Points of Interest with subsystem `com.moonlight.pyrowave`,
category `pipeline`. Exact signpost names and boundaries:

| Name | Interpretation |
| --- | --- |
| `Receive` | Synchronous peer receive/reassembly/validation, then app lock reacquisition, stats and scheduling tail; not pure parser time or initial app-lock wait. Fields: frameId, epoch, size; end result. |
| `Drawable` | `currentDrawable` acquisition only; valid flag. Tagged by epoch, **not frameId**; do not join it to a frame by invented identity. |
| `Upload` | No-copy alias creation or fallback payload memcpy/guard initialization, plus word-offset memcpy; frameId/generation, end `aliased=%d`. Not GPU transfer duration. |
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
| Scheduling equivalence | Four synthetic dequant strategies (serial/concurrent x per-band/batched; concurrent per-band uses disjoint single-slice views) compare bytewise across 12 cases; NOT EXECUTED, not pooled-pyramid mip-hazard or independent nonzero numerical/IDWT proof. |
| Numerical/pixel acceptance | Existing sparse/zero dispatch smoke and batching checks are **not** a full nonzero IDWT oracle. Full real-encoder nonzero corpus with independent reference planes, edges/aprons, quantization, chroma and agreed numeric bounds is prerequisite for FP16, fusion or hand-tuned SIMD. |
| Lifetime/live/performance | Metal validation, stop/resize/control races, final-reader lifetime, live encrypted transport/color/presentation and matched hardware profiling remain separate gates; CI smoke does not prove them. |

## Deferred / not implemented

- Persistent upload arena plus direct reassembly ownership: preserve identical/
  conflicting duplicate semantics, retirement/deadlines and peak-capacity budgets;
  pooled aliasing removes only the payload upload copy, not reassembly/offset copies.
- Multi-slot GPU overlap: define bounded last-reader leases for upload, pyramid
  and output resources; measure latency/backpressure, not just throughput.
- FP16 storage or arithmetic: require numeric goldens/error bounds from the full
  nonzero corpus; no precision or fast-math change is included.
- IDWT hand-tuned SIMD or fusion: prove apron/mirror ordering, quantization and
  chroma behavior against independent references before profiling acceptance.
- Offline metallib app-build/device/runtime validation remains pending; the
  one-entry in-process pipeline cache is unchanged, not a persistent disk cache.

## Apple Silicon follow-up audit (2026-10-27)

Active scope is native `Limelight/Stream/PyroWave`, not Qt/MoltenVK. This audit
records static source findings and ranked experiments, not a new architecture.

### Implemented, pending GPU verification

`MLPyrowaveMetalKernels.mm`'s `Pipeline` uses compute descriptors with
`maxTotalThreadsPerThreadgroup` of 128 for dequant and 64 for both IDWT variants;
only dequant promises `threadGroupSizeIsMultipleOfThreadExecutionWidth`.
This enables tighter compiler register/resource budgeting, not guaranteed
occupancy or speed. FP32 and fastMath=NO remain unchanged.

`MLPyrowaveMetalShaders.h` routes a uniform zero ballot (absent or coded-zero)
to early zero writes, avoiding scans, barriers and sign loads. The sentinel
ternary does not dereference UINT32_MAX. Zero stores are still required for
reused textures; this is not permission to leave stale coefficients unwritten.
`tests/pyrowave/test_metal_decoder.mm` now covers 12 synthetic combinations:
3/4 bands x 4x4/65x33 x mixed/absent/coded-zero. Zero cases require exact +0,
with poisoned unused record bytes in coded-zero fixtures. Existing nonzero
comparisons use the same pipeline, not an independent numerical reference.
All 12 cases compare four independently initialized/read-back outputs: serial per-band,
serial batched, concurrent per-band (disjoint aliased array views) and concurrent batched.
This exercises same-shader scheduling equivalence, not the actual pooled pyramid's
mip/slice hazards; that layout still requires GPU execution and Metal validation.
Evidence is static inspection only: no local CI-only GPU tests were run and
no performance gain was measured.

### Ranked next experiments

Priorities are provisional until a matched hardware trace identifies bottlenecks.
All filenames below refer to the native path above unless explicitly qualified.

1. **CPU receive allocator/copy cost.** `MLPyrowaveReassembly.c`'s Push now saves
   full wire bytes in a reusable bounded arena for exact duplicate validation,
   in addition to Common reassembly allocations
   (`moonlight-common/moonlight-common-c/src/PyrowaveReassembly.c`).
   `MLPyrowaveReceiver.cpp` constructs overwrite-initialized payload+16-byte storage;
   complete exact-length CopyFrame writes payload before ValidateOwned reads it,
   zeros only the 16 guards on success and moves ownership without another copy.
   `MLPyrowaveMetalDecoder.mm` aliases pooled payloads or memcpy's fallback input;
   the native word-offset table still copies. A bounded persistent arena/direct
   reassembly-ownership experiment could eliminate more work. Preserve byte-identical
   and conflicting duplicate semantics, initialized guards, expiry/replay,
   immutable frames and last-GPU-reader lifetime. Never substitute a collision-
   prone hash for duplicate byte checks, skip validation, or overwrite an in-flight
   upload. Upper-bound illustration only: 16 MiB * 120 fps * 2 (read+write) =
   3.75 GiB/s CPU traffic for one upload copy; not a workload measurement, and
   excluding other copies and cache effects.
2. **GPU dequant concurrency validation/benchmark.** EncodeDequant now issues 13
   concurrent dispatches writing disjoint component slices/mips with read-only
   input buffers. The unchanged encoder boundary and tracked heap resources
   preserve dequant-to-IDWT ordering; IDWT stays serial with texture barriers.
   IDWT LL writes overlap dequant subresources across levels: keep tracking and
   barriers. Actual pooled-pyramid Metal validation and matched profiling remain
   pending; no measured speedup. Further grouping needs a resource/layout contract.
3. **IDWT resource/bandwidth cost.** The FP32 pyramid feeds 13 IDWT dispatches,
   each with 64 threads and a 20x41 float2 shared tile: 6560 bytes before compiler
   padding. Measure occupancy, register spills, gather throughput and barrier
   stalls. SIMD32 specialization of the dequant scan fallback and IDWT shuffle/
   retile are candidates, not assumed wins; the compiler may already eliminate
   branches. Preserve apron, mirror and transposition behavior. R16Float
   intermediate storage could halve nominal coefficient storage versus R32Float,
   not total memory or GPU time. Require independent nonzero reference planes,
   quantization, edge and chroma error gates first; do not blindly flip half or
   fastMath.
4. **In-flight overlap.** The single pool stays busy through decode+render command
   completion. Two bounded leased slots could overlap CPU upload and GPU work,
   but require redesign of the 512 MiB memory budget and latency/drop policy.
   Preserve latest-pending behavior and final-reader lifetime; no unbounded queue.
   An extra slot could worsen latency. Source cadence and busy/expired counts
   must distinguish this bottleneck from insufficient input or deadline drops.
5. **Setup only.** The compute cache exists, but the renderer factory still
   compiles vertex/fragment functions and its render pipeline. Profile actual-
   renderer preflight/setup temporary allocation separately. A precompiled
   metallib or device-keyed render cache could reduce setup, not steady-state
   work. No blanket -Ofast/-mcpu=native or NEON rewrite of libc memcpy: shader
   optimization is independent of CPU flags. Retain deployment compatibility
   and runtime device checks.

### Compact acceptance plan

Use a matched arm64 Mac, OS/build and workload, covering dense+sparse inputs and
edge dimensions. Require Metal validation and existing CI GPU checks (exit 77
is a skip, never a pass); require an independent nonzero corpus before math or
layout changes. Follow the [profiling contract](#profiling-contract) and baseline
above: trace-off p50/p95/p99 CPU receive/upload/encode, GPU and actual-display
latency, plus drops, memory and thermal state; separate setup and trace overhead.
No runtime verification or performance claim is made by this documentation edit.
