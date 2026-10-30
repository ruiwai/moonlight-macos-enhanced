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
- Physical GC, CoreHID and AppKit relative reports submit immediately to the
  serial `inputQueue`; controller mouse emulation alone stays on the display link.
  GC keeps X/2.5 and -Y/2.5 scaling, then host sensitivity. Per-source fractional
  residuals replace min-one rounding; integral motion splits into signed-16 chunks
  (at most 4096 per report), stopping on enqueue failure without replay.
  Nonfinite/oversized reports reject and clear that source's residuals.
- Separate atomic motion/wheel epochs invalidate stale queued reports on lifecycle
  transitions; ordinary absolute/relative suppression resets only motion. Touch
  suppression transitions reset both; suppressed wheel resets only wheel. Residual clearing is lazy on `inputQueue`.
  Relative source switches clear motion residuals. CoreHID notifications have no
  timer/report-rate cap; the persisted legacy report-rate setting is inactive.
- AppKit NSEvent is the sole wheel source for every mouse driver, including GC;
  GC wheel callbacks are nil. Precision Touchpad suppression and input gates remain.
  `HIDMouseWheelPolicy.h` adapts SDL2 release-2.32.8 Cocoa lines 519–543 and
  SDL_mouse.c lines 878–927: X=-deltaX, Y=deltaY (not scrollingDelta*, no /10),
  nonprecise axes round away from zero. Per-axis float accumulators reset on
  opposite signs, extract trunc(integral), and retain fractional SDL residuals;
  precise fields contain only this event's normalized delta.
- Qt `app/streaming/input/mousewheel.cpp` classifies each axis independently:
  nonzero integral matching precise is discrete; otherwise nonzero precise is
  smooth, with integral fallback. Discrete sends are separate ±1-click entries,
  limited to 4096 per axis (4097 rejects that axis). Smooth finite amounts reverse,
  clamp to [-1,1] on macOS, then truncate ×120; zero amounts send nothing.
  Vertical precedes horizontal; first enqueue failure stops remaining sends.
  Reverse applies to each axis, ignoring direction-inversion metadata like Qt.
  No pacing, duplicate heuristics, speed/filter tuning, dominant-axis selection or
  smooth residual accumulation. Old tuning controls are hidden, schema retained.
  Trace/diagnostics record successful enqueues only as submitted; failures log zero
  dispatched amount. Native float/integral range guards reject unsafe conversions.
- Absolute coordinates are immutable per-submission snapshots, including repeats;
  native deduplication/deferred overwrite is removed. Common may coalesce only
  adjacent compatible absolute packets. Separate transport channels still do not
  guarantee strict host order; client enqueue success is not a host ACK.
- Tracked ordinary held-key `(VK, flags)` state changes only after successful
  Common enqueue. AppKit auto-repeat keyDown events are ignored before modifier
  synchronization; releases use the successful press pair; replacement
  aborts if the old release fails. Failed releases remain retryable by cleanup,
  which snapshots after preceding queued presses. Success is not a host ACK.

## Deliberate native boundaries and risks

- Native scaling/capture/free-mouse policy is not SDL pointer acquisition. Verify
  tiny reports, source changes, capture and large signed-16 splitting on devices.
- Wheel safety rejects nonfinite or signed-32-unrepresentable SDL accumulation;
  valid smooth deltas use Qt's macOS clamp, not platform acceleration passthrough.
  Enqueue failure can lose the rest of an event; neither axis is replayed later.
- Portable fixtures and static guards are not macOS/device or network validation.
  No live latency benchmark, universal latency improvement or losslessness claim.
- AppKit-only wheel delivery while captured/on multiple displays needs native validation.
  Multi-device CoreHID tasks serialize submission, not physical event timestamps.

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
CC=gcc python3 tests/input-coherence/motion.py
CC=clang python3 tests/input-coherence/motion.py
CC=gcc python3 tests/input-coherence/wheel.py
CC=clang python3 tests/input-coherence/wheel.py
python3 tests/input-coherence/check_native_wiring.py
python3 tests/input-coherence/keyboard.py
python3 tests/window-cleanup/shared_input.py
python3 tests/input-coherence/lifecycle.py
python3 -c 'from pathlib import Path; [compile(Path(p).read_bytes(), p, "exec") for p in ("tests/input-coherence/check_native_wiring.py", "tests/input-coherence/keyboard.py", "tests/window-cleanup/shared_input.py")]'
git diff --check
git -C "$common" diff --check
```

CTest includes policy, production-worker and atomic queue-interleaving checks,
plus existing Common regressions. Motion/wheel runners compile production portable
headers with undefined-behavior/float-cast sanitizers. Wheel covers ±multi-click
bursts, 4096/reject4097, two axes/reversal, SDL classification, fractions/clamp,
nonfinite/extreme inputs and first-failure stopping. All three native fixtures extract
production code on Linux but explicitly **skip** Foundation compilation/execution there;
exit zero on that skip is not a native runtime pass. Static wiring is text-based.
Lifecycle extraction covers queued wheel/absolute interleaving, no-relative touch
sessions, linger expiry and suppressed-wheel reset using a fake FIFO/touchpad.

## macOS / live checklist (pending, not established by Linux checks)

- Run keyboard, shared-input and lifecycle fixtures on macOS with clang/Foundation; verify queued
  keyboard enqueue-failure/retry cases, immediate GC normalization and all five
  deferred GC button FIFOs. GC scroll callbacks must remain nil on both axes.
- Build the native app with Xcode. On a real CS/KZ host, compare slow aim, rapid
  horizontal/vertical reversal, fractional reports, large swipes and movement around
  key/button/wheel edges; exercise 125/500/1000+ Hz mice and varied sensitivity.
- Check GC, CoreHID and AppKit paths, capture/recapture, absolute/touchscreen mode,
  touchpad suppression, discrete/precise wheels, simultaneous axes, fast clicks and
  rewritten scrolls; check controller emulation remains display-driven.
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
