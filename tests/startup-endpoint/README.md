# Startup endpoint regression checks

Run `python3 tests/startup-endpoint/run.py` on macOS with Xcode or Command Line Tools.

The checks execute the production stream target selection, endpoint resolver,
and cached-app host rebinding against a Foundation model. Endpoint enumeration,
settings, and UI adapters are isolated; no network requests or sessions are started.

Cases cover an empty active address on first launch and same-window retry,
manual route priority, an existing active route, disabled/offline routes,
IPv6 and custom port preservation, missing endpoints, and host UUID isolation.
The cached-app case verifies that later address updates reach the stream.
The first-launch case fails against the upstream RC5 selection code.

This change addresses client logs where first launch and retry selected a null
host, while closing and opening a new window selected the LAN address and showed
the first frame within about 2.5 seconds. Live hardware verification is pending.
