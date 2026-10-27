# PyroWave prerequisites (CI only)

## HUD event stats (portable CPU checks)
`test_stats.c` tests the app's pure `MLPyrowaveStats.h` accumulator in strict
C11 and C++14 with fatal ASan/UBSan: real-duration window rates, retained rates
across short windows, bytes/validated/GPU/presented separation, stalled freshness,
and zero/backwards-clock guards. CI runs it without Metal or exit-77 skips.
PyroWave bytes are inner callback traffic including fragments/FEC, not wire
bandwidth; decode timing is submission-to-GPU-completion wall time, not GPU time.
App locking, stop/epoch guards and actual presentation still require macOS checks.

## App draw gate (standalone CPU unit checks)
`test_draw_gate.c` exercises the production pure-C notification/admission helper:
coalescing, pending while GPU-busy, completion wakeup, no nil-drawable/Busy spin,
Dropped rescheduling and reset. The app still owns locking and stop/epoch guards.
CI runs C11 and C++14 with strict warnings and fatal ASan/UBSan before GPU steps,
without a hardware skip. These gate-only tests may also be run locally.
They do not prove AppKit callbacks, GPU correctness, performance or flicker fixes.

The [workflow](../../.github/workflows/pyrowave-prerequisites.yml) compiles and
runs the existing Common protocol/reassembly goldens and standalone native
metadata validation and strict reassembly adapter tests on `macos-latest`.
It runs on pushes to `pyrowave/**`, pull requests targeting `master` or `main`,
and manual dispatch. Only the native Common gitlink and its pinned `enet` nested
dependency are initialized; no application build or other nested dependency is needed.

## Dependency boundary

- Native Common: `9af0c8cc9be21afce5c735c7876ba03884f7d84c`
  (`moonlight-common/moonlight-common-c`).
- Foundation Common: `ddd52e20252ac2e27b4137b6db24530cf96fdfbb`.

At these pins, the protocol/reassembly helper sources are identical, but the
public API and streaming integration are incompatible. Helper equality is not
API/ABI compatibility: do not replace the native pin with Foundation Common.
The app has explicit `PyroWave (Experimental)` selection, offered only after actual-renderer preflight; default H.264 and indices 0..2 are unchanged.
See [Foundation testing](../../docs/pyrowave-foundation-testing.md): vanilla repository-local host `441b2b88051a00c91d562b24ac703adefc4cf87f`, not the older home-directory fork.
Policy assertions call the production offered-format helper; false preflight always returns H.264 for index 3, and legacy indices preserve their input masks.
These CPU policy checks do not prove GPU availability/correctness or host interoperability. Current native build/tests await Actions; unsupported-hardware exit 77 is a skip, not a pass.

## Common raw bridge /v2 (not app enablement)

Common now requires descriptor version 2 (rejecting v1) with full outer frame ID/RTP timestamp
and receive/presentation microseconds; received payload/descriptor are borrowed synchronously until callback return.
Common frees the packet; copy anything retained, never free borrowed storage or synchronously stop/destroy Common. Context survives stop through joins/cleanup; submissions may follow stop, never cleanup.
`submit` returns `int`: only `LI_PYROWAVE_PACKET_VALIDATED_FRAME` after synchronous owned complete PYRF recovery, protected TLV/codec and local-deadline validation satisfies the first-full-frame watchdog. Partial/malformed/expired/replayed input returns `LI_PYROWAVE_PACKET_IGNORED`; ignored/unknown results do not acknowledge a frame. This is not GPU/presentation success.
PyroWave bypasses the encrypted pre-auth frame filter: authenticate first, then require app full-ID/replay validation; AES-GCM and timeouts are unchanged. Watchdog routing/filter helper tests are synthetic; the new owned-validator composition gate below awaits macOS execution, not live encrypted/network evidence.
The CPU composition uses the production nonretaining `MLPyrowavePacketBridge` adapter, compiled separately as ARC Objective-C, forwarding into the actual coordinator. Nil sink/context/packet checks supplement exact IGNORED/VALIDATED_FRAME, isolation, ownership and stop cases. The app-boundary configuration helper is tested for each invalid field/FPS and valid edge limits; these are CPU shape checks, not GPU availability/budget proof. Native execution of this change is pending.
[`PyrowavePacketCallbackTest.c`](../../moonlight-common/moonlight-common-c/tests/PyrowavePacketCallbackTest.c) links actual Common
[`RtpVideoQueue.c`](../../moonlight-common/moonlight-common-c/src/RtpVideoQueue.c) / [`VideoDepacketizer.c`](../../moonlight-common/moonlight-common-c/src/VideoDepacketizer.c), checking exact identity/timing and selected-path gates.
Its arbitrary synthetic payload is not real codec data or validator composition.
[Public Common run 40390473131](https://github.com/ruiwai/moonlight-common-c/actions/runs/40390473131) passed one ASan/UBSan CTest on `macos-15` at historical pin `65f43ca`, after initial fixture UB was fixed.
Parent `f1c45a8` app build passed (run 40481438739); this does not establish live interoperability or readiness.
At exact Common `9af0c8cc9be21afce5c735c7876ba03884f7d84c`, [run 40403395078](https://github.com/ruiwai/moonlight-common-c/actions/runs/40403395078) passed both callback + negotiation CTests with fatal ASan/UBSan (`/tmp/pyrowave-9af0c8c-reviewed.log:249-258`). Live encrypted/network and actual callback → owned recovery/TLV/codec/deadline validator composition remain required.

## What CI checks

Common golden source lists and `src` includes match their CMake targets:

- `PyrowaveProtocolGoldenTest.c` + `PyrowaveProtocol.c`.
- `PyrowaveReassemblyGoldenTest.c` + `PyrowaveProtocol.c` + `PyrowaveReassembly.c`.
- `test_metadata.c` + `MLPyrowaveMetadata.c`, using the pinned Common header.
- `test_reassembly.c` + `MLPyrowaveReassembly.c` + `MLPyrowaveMetadata.c`
  + Common `PyrowaveProtocol.c` + `PyrowaveReassembly.c` (unchanged helpers).

None of these targets has extra compile definitions. CI uses `xcrun clang`, C11,
`-Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
-fno-omit-frame-pointer`, writes all four binaries to `RUNNER_TEMP`, and runs them
with sanitizer failures fatal. These are synthetic transport checks, not real
encoder bitstream conformance, codec decoding, GPU validation, or end-to-end
streaming validation. A green job does not qualify the feature for enablement.

## Metadata /v1 policy and coverage

The pure C validator consumes only recovered protected metadata; envelope
presence/geometry validation stays with its caller. The conditional app peer now uses it;
this section describes standalone CPU checks, not live streaming evidence. Output is required, cleared on entry and published only on complete
success; latency is a unique big-endian uint16 in 0.1 ms units, never inherited.
Both flag layers reject unknown bits and OPTIONAL/REQUIRED conflicts. Nonempty
regions need envelope PROTECTED, not entry PROTECTED. Reserved/unknown entries
(including flagless ones) skip unless either layer is REQUIRED. Known dynamic
HDR10+/Vivid/Dolby Vision/HLG peak entries always reject in this SDR-only slice.
Synthetic byte fixtures cover null/empty/reset, unaligned reads, latency extrema,
all truncated prefixes, bad lengths/flags, required/unknown/dynamic policy,
duplicates and atomic failure after valid latency, and the 65535-byte boundary.
They make no real encoder provenance claim. CI execution passed in run
40331107419; local lint is not compilation/test evidence.

## Next prerequisites

The standalone codec validator below is available for explicit CPU use only.
Real GPU-encoded fixtures and decoder hardening remain pending before
application/GPU integration or enablement; reassembly does not call this validator.

## Strict reassembly /v1 boundary and requirement map

Both modules now participate in conditional app wiring; standalone tests remain synthetic. The opaque
heap adapter reuses Native Common at the pin above, not Foundation's
streaming API; it never edits wire structs or accepts transport fragments.
Callers serialize calls and supply monotonic microseconds; zero is a valid start.
One active frame expires at first receive +100000 us (including Copy expiry).
All DATA bodies, even final/no-FEC tooling DATA, must be padded to B. Production
still requires XOR negotiation. FRAME_HEADER is optional and its body is retained
only for byte-identical duplicate checks, never as authoritative metadata.
Retirement retains a frame-ID watermark: equal/older and exact half-range IDs
are stale; strictly newer IDs recover after poison/expiry/copy (including wrap).
Malformed active input, conflicting descriptors/duplicates, or allocation errors
poison and clear storage. Identical duplicates do not extend the deadline.

On supported 64-bit platforms preflight caps protected bytes at 16 MiB, metadata
at 65535, packets at 65536, wireBlockCount*B at 40 MiB, and retained wire packets
plus pointer/length tables and separate FRAME_HEADER at 48 MiB. Accounting includes
64 bytes per allocation overhead; context + Common tables/padded blocks + saved
packets + temporary metadata + caller output stays below 128 MiB. Accepted output
capacity is <=16 MiB. Runtime byte checks supplement preflight; no unbounded queue.
This accounts allocation requests, not process RSS/allocator internals.

Map from local design `docs/pyrowave-protocol-contract.md` requirements (not a tracked remote document):
- Outer transport/envelope: `testReorderCopyReplay`, `testDuplicates`,
  `testDescriptorsAndPadding`, `testMalformedAndBudgets` cover missing header,
  reordering, all three duplicate kinds/flags, padding/timestamp/geometry conflicts,
  truncation/trailing/malformed input, poison/newer recovery and preflight bounds.
- Deadlines/bounds/loss: `testIds`, `testDeadlines`, `testFec` cover stale/wrap/half,
  exact expiry/overflow/copy expiry, duplicate non-extension, partial no-copy,
  capacity retry, copy once, one/two XOR losses and conflicting recovered originals.
- Protected metadata: `testMetadata` covers cross-block recovery, untrusted header
  copy, valid/invalid metadata, zero outputs/unchanged sentinel bytes on failure,
  replay and no carryover; `test_metadata.c` covers TLV flags/length/type/max rules.
- Composite/fuzz progress: `test_pipeline.cpp` adds CPU composition and finite
  deterministic parser/wire mutation smoke (baseline CI passed, not callback composition); a coverage-guided
  fuzz campaign and real encoder/GPU acceptance remain unfulfilled requirements.
- Live negotiation interoperability, real encoder compatibility, codec decoding, and GPU
  lifetime/validation remain pending; synthetic fixtures prove none of these.
Allocation failures are handled but not fault-injected/tested; reassembly CI passed
in run 40334474521. Local lint is not test execution.

Local validation for this candidate is lint only; do not build or execute the
golden tests locally:

```sh
nix shell nixpkgs#actionlint -c actionlint .github/workflows/pyrowave-prerequisites.yml
```

## Codec /v1 (synthetic only)

`MLPyrowaveCodec.hpp/.cpp` defines an independent C++14 CPU transaction, not the
upstream unsafe parser or a Metal ABI. CI links only that cpp and `test_codec.cpp`,
using Common `src` and native PyroWave includes for reassembly scalar types; no C
functions are referenced. The same fatal-sanitizer step compiles with `xcrun
clang++ -std=c++14 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
-fno-omit-frame-pointer` and runs it. No Xcode, GPU, or streaming wiring is added.

Only even 2..16384 SDR709/8-bit/420 sessions are supported; wire range full/limited
and siting center/left must match. Input is 8 bytes..16 MiB. Five-level aligned
layout capacity is preflighted with checked uint64 arithmetic before allocation.
Input+16 zero guards, offsets and session-cached masks (4/2 bytes per slot),
object sizes/1024-byte bookkeeping must fit 32 MiB, using vector capacities
(allocation requests, not process RSS).
Offsets are BYTE offsets to unique record headers; UINT32_MAX is absent. Guard
bytes are initialized, but shader read safety is not certified. Receiver recovery
copies once into owned scratch; `ValidateOwned` validates then moves it without
a second payload copy. The borrowed-input wrapper still copies after validation.
Immutable results snapshot envelope ID/timestamp/timing/metadata; failures publish
nothing. Masks are cached per session; payload sign counts use OR/popcount.
No clock comparison is performed in the codec; callers own expiry decisions.

Synthetic CHECK fixtures cover sparse/zero counts, ownership, both ranges/sitings,
2/128/130 and CPU-only maximum dimensions, sequence/color/session conflicts,
duplicate identity and zero-word termination, geometry, control/plane/sign bounds,
quant167/168/200, excess/truncated/trailing bytes, input preflight, and atomic failure.
Quant <=167 is an explicit pinned encoder-domain policy; zero control with nonzero
q low nibble rejects the dequant/offset-scan ambiguity. Unused sign/padding values
are ignored, but duplicate records must match every byte. Full-layout coverage is
not required: unique transmitted count must equal the sequence's declared count.

Algorithm provenance: Foundation codec `cc91cbf6363721d19d135d05ca8e3d8665ea6afd`,
MIT AND GPL-3.0-only; notices are retained in the new source. These synthetic checks do NOT establish encoder
conformance, pixel accuracy, GPU validity, interoperability, or feature readiness.
Codec CI passed in run 40337167846; local evidence remains lint only.

## Pinned CPU packetizer oracle /v1

`test_packetizer.cpp` uses AlkaidLab/pyrowave (Foundation codec pin):
`cc91cbf6363721d19d135d05ca8e3d8665ea6afd`.
CI checks out `.ci-pyrowave-codec` without submodules or credentials persistence
and asserts HEAD before compiling. Only upstream `metal/pyrowave_bitstream.cpp`
is compiled separately with C++14, `-Wall -Wextra` and the same sanitizers (no
`-Werror` for existing upstream warnings). Native validator/test compilation keeps
`-Werror`; linking adds only that upstream object. Fatal sanitizer execution stays
in the same macOS job. No CMake, nested dependencies, GPU/Metal/Vulkan calls,
unsafe `BitstreamParser` calls, or application integration are involved.

Hand-authored coefficient records exercise exact pinned `BlockLayout::init`,
`compute_num_packets` and `packetize` CPU serialization, NOT GPU generation.
Independent literal zero/sparse 128x128 goldens check exact output bytes;
metadata initializes every slot, uses word offsets and provides a readable aligned
sequence5 dummy header even at zero count. Packet offsets/sizes/exhaustion and
boundary16/24 split/concatenate cover separated sparse slots including the last.
These packets are not PYRF frames; only complete pure bytes reach the validator.
Checks cover owned byte identity, full envelope identity/timing/latency, byte-offset
tables/absent slots, zero guards, and capacities51/51/93/3261/393216 for
2x2/128x128/130x130/1920x1080/16384x16384 sequence-only frames. Each distinct
mapping geometry in 2/130/1920 layouts accepts its full legal ballot, with pooled
signs, and rejects a structurally complete outside-bit record where one exists
(full4x4 has none). Literal mutations reject dimensions/count/code/sequence,
truncation and conflicting duplicates; nonzero unused padding remains legal.
Snapshot output is produced/compared only on macOS execution; oracle run
40339618731 passed. No local generation, compilation or execution is claimed.
Local evidence is selected actionlint and exhaustive cppcheck on the new test.
This is a pinned CPU serialization/layout oracle, not real GPU-encoded corpus,
pixel decoding, transport interoperability, shader safety or feature readiness.
Real GPU-encoded corpus and GPU validation remain deferred.

## CPU composition /v1 (synthetic baseline CI passed)

Observed prerequisite CI runs: baseline Common goldens 40329294718, metadata
40331107419, strict reassembly 40334474521, codec 40337167846, and pinned CPU
packetizer oracle 40339618731 all passed. Those historical runs did not execute
the composition test; parent baseline `6e97b2f` run 40401794573 subsequently passed its synthetic CPU composition/mutation smoke, not raw callback composition.

`test_pipeline.cpp` links actual Common protocol/reassembly, native metadata/
reassembly C11 objects and the C++14 codec, without mocks, upstream headers, GPU,
Xcode or live wiring. The synthetic fixed 128x128 sequence5/index0 literal matches
`test_packetizer.cpp`'s pinned CPU sparse golden, not a captured encoder frame.
A protected optional HOST_LATENCY TLV plus codec gives 34 protected bytes; padded
B16/N3 DATA and XOR parity recover a missing metadata/codec boundary shard without
FRAME_HEADER. Checks cover publication gating, exact bytes/offsets/guards, full
identity/timing/session/latency, ownership after destruction, two losses, expiry,
invalid codec versus invalid metadata, newer recovery and successful no carryover.

A fixed-seed in-test PRNG drives 1024 codec/TLV cases (inputs <=128 bytes) and
256 fresh-context wire cases using valid seeds, bit mutations, truncation and
trailing bytes. Accepted results must own exact bytes; failures publish nothing,
and inputs remain unchanged. Packet/output buffers are fixed <=256 bytes;
production allocation caps still bound mutated geometry. CI compiles C and C++
separately with strict warnings/ASan/UBSan, then runs in a fatal-sanitizer step
limited to 3 minutes inside the 10-minute job. Baseline CI passed as recorded above; historical local checks
are selected actionlint and exhaustive C++14 cppcheck only. This is finite smoke,
not coverage-guided fuzzing, a fuzz proof, transport interoperability, pixel/GPU
validation, or permission to enable PyroWave.

## Common negotiation /v1 (Common CI passed; not app enablement)

Introduced at `e26d480` and retained at the current pin, actual Common RTSP uses a bounded five-attribute decimal SDP parser,
retains the negotiated contract per context, emits selected SDR ANNOUNCE fields
and enforces negotiated inner-packet size on the raw callback path.
[Common run 40400754728](https://github.com/ruiwai/moonlight-common-c/actions/runs/40400754728) passed two actual-Common callback + negotiation CTests on `macos-15`, ASan/UBSan fatal.
Historical `5303fde`: attempt 1 had no runner; attempt 2 callback passed, negotiation fixture lacked `VideoPortNumber`; `e26d480` fixes only that fixture.
Tests link actual Common helpers/serializer with synthetic inputs, not network RTSP.
The two production units have explicit Common Xcode membership; Apple-only tests
are not app sources; no PBX change is needed for this repin. Historical `a8a647d`
CI was all green with `65f43ca`; parent baseline `6e97b2f` subsequently passed all three workflows (PyroWave, Input 12/12 CTests, macOS app build/draft), compiling both Common units once (`/tmp/pyrowave-6e97b2f-404017*-reviewed.log`). At `f1c45a8`, app run 40481438739 passed with the current Common pin. Current callback/coordinator, preflight, Metal presentation and experimental selection are implemented;
those historical runs do not validate this snapshot or prove live interoperability.

## Actual Common → coordinator → watchdog CPU gate (execution pending)

`test_common_coordinator.mm` and its opaque C transport helper link the full Common
Debug dylib, not mocked transport or duplicate standalone Common helper objects.
CI builds only pinned `enet`, discovers installed `openssl@3` with Homebrew, and
runs the two Common callback/negotiation CTests plus this fatal-ASan/UBSan target.
ASan shadow checks confirm Common freed source envelopes before return; retained
frames preserve bytes after teardown. Synthetic PYRF/XOR covers full identity/timing/latency,
partial/malformed/codec/expiry/replay/stop rejection and opposite-TLS isolation.
Direct depacketizer times are deterministic; actual `RtpvAddPacket` uses the same
`LiGetMicroseconds` clock as the coordinator. Watchdog reset is observation only.
No GPU skip path, encrypted network, sockets, live app wiring or enablement claim.

## Metal decoder synthetic smoke

`test_metal_decoder.mm` links the actual decoder, kernels, coordinator, codec/receiver
and explicit existing C objects in an independent fatal-ASan/UBSan CI step (3 minutes).
CPU factory-invalid checks precede device detection; unsupported macOS 13/Apple7
hardware returns 77, visibly skipped in the summary. Supported factory/submit failures fail.
A minimal copied coordinator fixture produces an immutable validated sparse 128x128 frame.
Checks cover resource creation/error clearing, 16384-square budget rejection, nil inputs,
generation mismatch, presentation refusal/no completion, submission/successful bounded
completion and reuse, exact deadline expiry, permanent clock-regression stop and idempotent stop.
Presentation blits all three correctly shaped R8Unorm planes to a shared buffer with
256-byte rows; it never retains planes/command buffers or reenters decoder controls.
This is synthetic dispatch/lifetime smoke, NOT pixel correctness, Metal validation,
real encoder conformance, deterministic Busy coverage, streaming or feature readiness.

Dequant batches bands with grid.z=3/4 per component/level: 42 dispatches become 13;
IDWT remains 13, caches pipeline bindings and sets shared push bytes once per level.
Synthetic bytewise per-band/batched dequant comparisons cover sparse/nonzero inputs,
3/4 bands and 4x4/65x33 geometry, not a full nonzero IDWT/real-encoder pixel oracle.

`test_metal_kernels.mm` additionally checks same-device bundle identity, stale-error
clearing and nil rejection after a hit without evicting the last good cache entry.
The serialized factory retains only one most-recent successful immutable bundle
by exact device object identity; callers keep older returned bundles alive.
This removes repeated preflight/setup compilation, not measured steady-state work.
All existing CI gates remain required; new Metal assertions are CI-only and pending.
See [optimization v1](../../docs/pyrowave-metal-optimization.md) for profiling labels,
baseline/acceptance gates and deferred work. No local benchmarking hardware or
speedup evidence is available; local golden/Metal execution remains prohibited.

## Standalone SDR Metal renderer (not live enablement)
`MLPyrowaveMetalRenderer` is an opt-in peer consuming public coordinator packets,
on the supplied existing renderer queue/device only; conditional Connection/
VideoDecoderRenderer wiring and explicit preflight-gated selection now exist. Fixed session, permanent stop, one pending + one
render-local owner; decoder Busy drops the consumed frame without retaining a retry.
Nil/invalid targets preserve pending. BGRA8Unorm only, aspect-fit full-target black clear.
Non-memoryless storage (stored result required). Fractional viewport; texture-bounded
inward scissor uses ceil(left/top), floor(right/bottom); empty fit preserves pending.
BT.709 full/limited range and center/left chroma; no sRGB/gamma transform. Caller
layer MUST use `kCGColorSpaceITUR_709` with SDR/EDR off. Caller accounts external
targets within the shared 512 MiB session budget and cannot reuse/read them
until completion; command-buffer references retain resources. Decoder alone commits.
Clock/context must outlive the renderer and be thread-safe/nonreentrant.
The 3-minute strict C++14 ARC Werror ASan/UBSan step links actual sources and explicit
existing C/C++ objects with Foundation/Metal/QuartzCore. CPU-invalid checks
precede hardware detection; unsupported hardware visibly returns 77.
Supported failures abort. Common serialization feeds valid header-only zero-count
frames (padded B16/N1/XOR), repeats increasing IDs in all range/siting
configs, checks invalid-target pending preservation, bounded completion and stop.
Test-only same-queue blit checks gray within two codes of R8=128 and black letterbox;
foreign-device checks need a second device. Uniform frames do NOT verify siting,
nonzero coefficients, encoder pixel correctness or Metal validation.
UI/drawable and Busy timing untested; offscreen shares pass. Explicit opt-in is experimental, not proven GPU correctness or interoperability.
The app bounds the drawable canvas to both source and backing dimensions (no
upscale); the compositor scales it to the window. Preflight/setup reserve two
maximum source-size canvases, not full Retina backing. The hard cap is 512 MiB,
not a full-cap allocation; actual CPU/heaps/two-target charges must still fit.
CPU selector cases include 1080p/1440p/4K on 4K/5K/small backings before the GPU
probe. A separate drained-session hardware loop creates actual SDR renderers for
720p/1080p/1440p/4K and checks source-size presentation admission, with no submissions.
Supported-device failures fail; this is allocation smoke, not pixel/UI correctness.
