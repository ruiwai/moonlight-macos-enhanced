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

Codec parser/decoder hardening with malformed and real encoder fixtures remains
pending before application/GPU integration or enablement. Copied codec bytes are
not codec-validated: they still require a codec validator, not immediate decoding.

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
- Signal/color, codec parser, real encoder compatibility, negotiation and GPU
  lifetime/validation remain pending; synthetic fixtures prove none of these.
Allocation failures are handled but not fault-injected/tested; CI execution is pending. Local lint is not test execution.

Local validation for this candidate is lint only; do not build or execute the
golden tests locally:

```sh
nix shell nixpkgs#actionlint -c actionlint .github/workflows/pyrowave-prerequisites.yml
```
