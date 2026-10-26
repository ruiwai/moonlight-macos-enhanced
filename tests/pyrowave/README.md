# PyroWave prerequisites (CI only)

The [workflow](../../.github/workflows/pyrowave-prerequisites.yml) compiles and
runs the existing Common protocol and reassembly golden tests on `macos-14`.
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

The source lists and `src` include directory match the Common CMake targets:

- `PyrowaveProtocolGoldenTest.c` + `PyrowaveProtocol.c`.
- `PyrowaveReassemblyGoldenTest.c` + `PyrowaveProtocol.c` + `PyrowaveReassembly.c`.

Neither target has extra compile definitions. CI uses `xcrun clang`, C11,
`-Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
-fno-omit-frame-pointer`, writes both binaries to `RUNNER_TEMP`, and runs them
with sanitizer failures fatal. These are synthetic transport checks, not real
encoder bitstream conformance, codec decoding, GPU validation, or end-to-end
streaming validation. A green job does not qualify the feature for enablement.

## Next prerequisites

Add adversarial duplicate/conflicting-packet, timestamp/wraparound, and TLV
validation tests, then codec parser/decoder hardening with malformed and real
encoder fixtures before considering application/GPU integration or enablement.

Local validation for this candidate is lint only; do not build or execute the
golden tests locally:

```sh
nix shell nixpkgs#actionlint -c actionlint .github/workflows/pyrowave-prerequisites.yml
```
