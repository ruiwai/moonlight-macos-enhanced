# PyroWave prerequisites (CI only)

The [workflow](../../.github/workflows/pyrowave-prerequisites.yml) compiles and
runs the existing Common protocol/reassembly goldens and standalone native
metadata validation tests on `macos-15`.
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

None of these targets has extra compile definitions. CI uses `xcrun clang`, C11,
`-Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
-fno-omit-frame-pointer`, writes all three binaries to `RUNNER_TEMP`, and runs them
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

Frame/reassembly duplicate/conflicting-packet and timestamp/wraparound hardening
remain deferred, followed by codec parser/decoder hardening with malformed and real
encoder fixtures before considering application/GPU integration or enablement.

Local validation for this candidate is lint only; do not build or execute the
golden tests locally:

```sh
nix shell nixpkgs#actionlint -c actionlint .github/workflows/pyrowave-prerequisites.yml
```
