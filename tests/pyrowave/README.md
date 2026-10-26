# PyroWave prerequisites (CI only)

The [workflow](../../.github/workflows/pyrowave-prerequisites.yml) compiles and
runs the existing Common protocol/reassembly goldens and standalone native
metadata validation and strict reassembly adapter tests on `macos-15`.
It runs on pushes to `pyrowave/**`, pull requests targeting `master` or `main`,
and manual dispatch. Only the native Common gitlink is initialized; nested
submodules and the application build are not needed.

## Dependency boundary

- Native Common: `3ccd50806d2bb0446db61b88501b6f7609daa89d`
  (`moonlight-common/moonlight-common-c`).
- Foundation Common: `ddd52e20252ac2e27b4137b6db24530cf96fdfbb`.

At these pins, the protocol/reassembly helper sources are identical, but the
public API and streaming integration are incompatible. Helper equality is not
API/ABI compatibility: do not replace the native pin with Foundation Common.
This prerequisite adds no application integration and leaves PyroWave disabled.

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
presence/geometry validation stays with its caller. It is not wired into Xcode
or streaming. Output is required, cleared on entry and published only on complete
success; latency is a unique big-endian uint16 in 0.1 ms units, never inherited.
Both flag layers reject unknown bits and OPTIONAL/REQUIRED conflicts. Nonempty
regions need envelope PROTECTED, not entry PROTECTED. Reserved/unknown entries
(including flagless ones) skip unless either layer is REQUIRED. Known dynamic
HDR10+/Vivid/Dolby Vision/HLG peak entries always reject in this SDR-only slice.
Synthetic byte fixtures cover null/empty/reset, unaligned reads, latency extrema,
all truncated prefixes, bad lengths/flags, required/unknown/dynamic policy,
duplicates and atomic failure after valid latency, and the 65535-byte boundary.
They make no real encoder provenance claim. Execution is CI-only and pending
until the updated workflow runs; local lint is not compilation/test evidence.

## Next prerequisites

The standalone codec validator below is available for explicit CPU use only.
Real GPU-encoded fixtures and decoder hardening remain pending before
application/GPU integration or enablement; reassembly does not call this validator.

## Strict reassembly /v1 boundary and requirement map

Both modules remain standalone/disabled, outside Xcode/live streaming. The opaque
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

Map from [protocol contract](../../docs/pyrowave-protocol-contract.md) requirements:
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
- Negotiation, real encoder compatibility, codec decoding, and GPU
  lifetime/validation remain pending; synthetic fixtures prove none of these.
Allocation failures are handled but not fault-injected/tested; CI execution is pending. Local lint is not test execution.

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
Input+16 zero guards +4*capacity offsets +2*capacity temporary masks + fixed
object/1024-byte bookkeeping allowance must fit 32 MiB (requests, not process RSS).
Offsets are BYTE offsets to unique record headers; UINT32_MAX is absent. Guard
bytes are initialized, but shader read safety is not certified. The immutable
result copies the full input and envelope ID/timestamp/timing/metadata; failures
publish nothing. No clock comparison is performed; callers own expiry decisions.

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
CI execution is pending; local evidence is lint only, not compilation or test runs.

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
Snapshot output is produced/compared only on macOS execution, which is pending
until CI runs; no local generation, compilation or test execution is claimed.
Local evidence is selected actionlint and exhaustive cppcheck on the new test.
This is a pinned CPU serialization/layout oracle, not real GPU-encoded corpus,
pixel decoding, transport interoperability, shader safety or feature readiness.
Real GPU-encoded corpus and GPU validation remain deferred.
