# Guide for JackBridge Development

JackBridge is a macOS JACK ↔ CoreAudio bridge. A user-space `AudioServerPlugIn` HAL driver presents a virtual audio device; a userland daemon shuttles buffers between JACK and the driver via POSIX shared memory.

The downstream use case driving this fork: a Raspberry Pi running JACK + netJACK2 as a network audio interface for macOS DAWs (Logic, Pro Tools, REAPER). See `PLAN.md` for the revival roadmap and `docs/` for everything else.

## Repo layout

```
daemon/      JACK client + shm publisher (built as the JackBridged Xcode target)
driver/      AudioServerPlugIn HAL bundle (Xcode project; both targets live here)
shared/      IPC contract header (JackBridge.h) + logging shim (jb_log.hpp)
tools/       chkshm / rmshm shm-inspection utilities + jackbridge-ctl
installer/   build-pkg.sh, launchd plists, postinstall, route helpers, config.plist
pi/          Raspberry Pi side: install.sh + jackd/netJACK2 systemd helpers
docs/        Architecture, codebase tour, idiosyncrasies, setup
reinstall.sh One-shot: install latest pkg, wipe shm, bounce coreaudiod + agents
```

Source of truth: `daemon/`, `driver/JackBridge/Plug-In/`, `shared/JackBridge.h`.

## Build

The whole tree (driver bundle + `JackBridged` daemon + helper bins) builds and packages via:

```bash
./installer/build-pkg.sh                 # → installer/build/JackBridge-<version>.pkg
./reinstall.sh                           # install pkg, rmshm, killall coreaudiod, bootcycle agents
```

For driver-only iteration you can `open driver/JackBridgePlugIn.xcodeproj` and build the `JackBridgePlugIn` / `JackBridged` targets directly, but `reinstall.sh` is the canonical inner loop because protocol-version bumps require the shm wipe + agent bootcycle dance.

## Architecture in 30 seconds

```
JACK process callback                CoreAudio IO proc
        │                                    │
        ▼                                    ▼
   daemon writes/reads ──── shm ────► driver memcpy in/out
        │                                    │
        ▼                                    ▼
   jackd / netJACK2                  DAW / system audio
```

Two processes, one shared-memory region (`/JackBridge`), two ring buffers (in + out). Both sides run in the same CoreAudio host-clock domain — see `docs/architecture.md` for why and what that constrains.

## Key idiosyncrasies (do not be surprised by these)

- **IPC contract header** lives at `shared/JackBridge.h`. Both targets pick it up via `HEADER_SEARCH_PATHS=$(SRCROOT)/../shared`. Bump `JACKBRIDGE_PROTOCOL_VERSION` on every shm layout change — the cooperative handshake in `check_protocol_version()` refuses to attach on mismatch, so you also need `./reinstall.sh` (or a manual `rmshm`) to clear the stale region.
- **Hardcoded `*2` and `8`-byte-per-frame literals** throughout assume stereo float. Don't generalize without auditing every site.
- **`tools/rmshm.c` also unlinks legacy `/jackrouter` + `/jackrouter2` names** — intentional, helps users migrating from the upstream `madhatter68/JackRouter` install.

Full list with file/line citations: `docs/idiosyncrasies.md`.

## Development principles for this fork

- **Same-clock-domain assumption is load-bearing.** Mac jackd must run with the CoreAudio backend; netJACK2 runs as an internal JACK client and handles the Pi↔Mac clock crossing itself. Do not add SRC to JackBridge — that's netJACK2's job. See `docs/architecture.md`.
- **Realtime safety in audio paths.** No allocation, no syscalls, no logging, no locks in the HAL IO proc or the daemon's JACK process callback. Ring-buffer memcpy only.
- **Apple Silicon first.** All shm synchronization uses `std::atomic` with explicit `memory_order_acquire` / `release`. No `volatile`-as-synchronization. Test on arm64 hardware, not just under Rosetta.
- **Fail loud, not silent.** Heartbeat + version-stamp on the shm region. Refuse to attach on mismatch. Flip `kAudioDevicePropertyDeviceIsAlive=0` when daemon goes away — DAWs disconnect cleanly instead of getting forever-silence.
- **Pragmatic over perfect.** This is a prototype being hardened, not a clean-slate rewrite. The shm IPC layout, single-device assumption, and 2-in/4-out scope are *fine for the use case* — don't grow them speculatively.

## Codesigning + macOS specifics

Driver and daemon both need Developer ID signatures, hardened runtime, and notarization inside a `.pkg` — `installer/build-pkg.sh` drives that (notarization is skipped unless `NOTARY_PROFILE` is set). Daemon needs `com.apple.security.cs.disable-library-validation` to `dlopen` libjack. Install path is `/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`; the daemon + helpers land in `/Library/Application Support/JackBridge` and run under LaunchAgents (`com.jackbridge.jackd`, `com.jackbridge.daemon`).

Details: `docs/macos-setup.md`.
