# CS/KZ input-coherence port

## Provenance and scope

- Source: `~/moonlight-qt`, HEAD `1b48cf21`.
- Relevant retained changes: `9a5c6a74` (coherence), `0d5a97ac` (wheel
  semantics), `4b7b3d6e` (dispatch/queue work); respect removal `e1af5468`.
  These are provenance, not wholesale cherry-picks into the native client.
- Common reference: `ec2a6f3` (`ec2a6f392d2c214ad4c977b6c08bfeb45680ef5c`),
  cached at `/tmp/moonlight-common-c-ref.bRdS0axJ` during review.
  Adapted to target pin `3ccd50806d2bb0446db61b88501b6f7609daa89d` and its
  per-connection `Ctx` native fork; **not** a wholesale Common replacement.
  Existing native transport, scroll tracing, pen/touch/controller and protocol
  extensions remain; Qt SDL dispatcher code does not apply to AppKit/GC input.
- Removed Qt ordered-input negotiation/settings and input telemetry are excluded;
  existing native diagnostics are not a new imported telemetry feature.

## Implemented behavior

- Relative and absolute packets carry submission-time snapshots rather than
  shared mutable mouse accumulators. Absolute coalescing takes only adjacent
  absolute snapshots; intervening input remains a boundary.
- Relative producer tail merging and worker batching combine adjacent compatible
  motion within signed-16 limits. Nonzero horizontal sign reversals and arithmetic
  overflow end a segment; horizontal reversal packets remain distinct.
  Vertical same-segment cancellation follows the source: this is not preservation
  of every motion report or every vertical reversal.
- Continuous relative motion remains throttled. Ready non-motion, reversal and
  overflow boundaries flush the current segment without the normal batching wait;
  semantic packets and motion immediately following a boundary avoid that delay.
- Keyboard, button and both scroll-axis packets are flush barriers. At a full
  queue, their admission may evict the oldest queued relative-motion packet,
  leaving retained entries in FIFO order. A semantic-only full queue still rejects;
  allocation, shutdown and send failures remain possible. No all-input-losslessness
  claim is made. Queue classification/removal, merging and eviction are mutex-atomic.
- Immediate physical GameController (GC) motion dispatch was withdrawn: applying
  minimum-one-pixel scaling per report changes slow aim for tiny/high-rate reports.
  Baseline physical GC callbacks accumulate X and inverted Y until display; that
  frame total uses the existing 2.5 divisor, sensitivity, min1/clamp and display-path
  capture/absolute/suppression policy. No residual design or sensitivity change.
  Controller mouse emulation also remains on the display link.
- GC buttons and GC/AppKit scroll submissions retain the existing serial input
  queue wrappers and failure logs; scroll trace/send values are captured before
  dispatch. Native GC frame aggregation can still cancel reversals or cross
  key/button/wheel events **before** Common queue submission. Common FIFO cannot
  fix that upstream boundary: native motion/key ordering is not guaranteed.
  Separate transport channels also do **not** guarantee strict host ordering.
- Tracked ordinary held-key `(VK, flags)` state changes only after successful
  Common enqueue. Repeats/releases use the successful press pair; replacement
  aborts if the old release fails. Failed releases remain retryable by cleanup,
  which snapshots after preceding queued presses. Success is not a host ACK.

## Deliberate native boundaries and risks

- Qt integer/precise wheel classification and splitting were not transplanted:
  native hardware classification, speed, duplicate suppression, synthetic-event
  and gesture semantics remain as-is. This does **not** guarantee that all native
  wheel clicks are preserved. Test both axes, fast bursts and direction changes.
- Qt large-relative splitting was not transplanted. The native `CGFloat` to
  `short` scaling/clamp policy remains, so oversized native deltas can clamp.
- Native GC retains frame aggregation and minimum-pixel rounding, not per-report
  delivery or reversal/edge preservation. Verify restored slow aim and frame-rate
  dependence at multiple sensitivities/report rates; Common sees only frame totals.
- Portable fixtures and static guards are not macOS/device or network validation.
  No live latency benchmark, universal latency improvement or losslessness claim.

## Reproducible portable checks (from repository root)

Requires CMake, C11 GCC/Clang, pthreads, OpenSSL development files and the target's
matching initialized ENet submodule (not a system ENet replacement). Use an
out-of-tree build; worker tests stub transport/clock, not a live host.

```sh
common=moonlight-common/moonlight-common-c
build=/tmp/cs-kz-input-build
cmake -S "$common" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build" -j4
ctest --test-dir "$build" --output-on-failure
python3 tests/input-coherence/check_native_wiring.py
python3 tests/input-coherence/keyboard.py
python3 tests/window-cleanup/shared_input.py
python3 -c 'from pathlib import Path; [compile(Path(p).read_bytes(), p, "exec") for p in ("tests/input-coherence/check_native_wiring.py", "tests/input-coherence/keyboard.py", "tests/window-cleanup/shared_input.py")]'
git diff --check
git -C "$common" diff --check
```

CTest includes policy, production-worker and atomic queue-interleaving checks,
plus existing Common regressions. Both Python native fixtures extract production
code on Linux but explicitly **skip** Foundation compilation/execution there;
exit zero on that skip is not a native runtime pass. Static wiring is text-based.

## macOS / live checklist (pending, not established by Linux checks)

- Run both fixture commands above on macOS with clang/Foundation; verify queued
  keyboard enqueue-failure/retry cases, physical GC frame accumulation/scaling and
  all five deferred GC button press/release FIFOs (not native motion/key order).
- Build the native app with Xcode. On a real CS/KZ host, compare slow aim, rapid
  horizontal reversal, vertical cancellation, large swipes and movement around
  key/button/wheel edges; exercise 125/500/1000+ Hz mice and varied sensitivity.
- Check GC, CoreHID and AppKit paths, capture/recapture, absolute/touchscreen mode,
  touchpad gestures, discrete/precise wheels, horizontal scroll, duplicate/synthetic
  suppression and controller mouse emulation. Do not assume every wheel click lands.
- Stress queued motion, alternating signs and signed-16 limits; admit button/key
  releases and both scroll axes under saturation, and verify semantic-only rejection.
  Change keyboard layout while held; test repeat, failed enqueue/release cleanup,
  teardown/reconnect and concurrent windows/context isolation with stale GC owners.
- Observe host actions and stuck-key recovery across transport conditions; client
  FIFO tests alone cannot establish cross-channel host order or latency.

## Import / dependency handoff

- Native Common is pinned to `77bd732ff3d8b034f1fc6937e89ac0585d83fa0d`, public at
  https://github.com/ruiwai/moonlight-common-c/commit/77bd732ff3d8b034f1fc6937e89ac0585d83fa0d
  on branch `input/cs-kz-coherence`. The gitlink contains the Common implementation,
  including its policy header and policy/worker/atomic-queue tests.
- App code and fixtures were imported byte-for-byte from **uncommitted** work in
  `/home/hilaolu/moonlight-macos-enhanced-cs-kz` at HEAD
  `399581ca3247d6ccc769bd21c670f7631d40db27`, not from that commit's tree.
  This document retains the source port notes with an updated handoff.
- macOS fixture execution, app build and the live checklist above remain pending;
  adding CI is not evidence of a runtime pass. No physical motion/key-order or
  latency guarantees are made.
- Existing `inputQueue` blocks capture a raw Common context; teardown lifetime is
  **not proven**. Require queued-backlog/drain, teardown and reconnect testing,
  including concurrent windows and stale callbacks, before lifetime claims.
