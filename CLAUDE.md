# Guide for JackBridge Development

JackBridge is a macOS JACK ↔ CoreAudio bridge. A user-space `AudioServerPlugIn` HAL driver presents a virtual 4-in / 2-out audio device at 48 kHz; a userland daemon shuttles buffers between JACK and the driver via POSIX shared memory.

The downstream use case driving this fork: a Raspberry Pi running JACK + netJACK2 as a network audio interface for macOS DAWs (Logic, Pro Tools, REAPER). See `README.md` for the user-facing overview, `pi/README.md` for the pi side, and `docs/` for architecture and gotchas.

## Repo layout

```
daemon/      JACK client + shm publisher (Xcode target: JackBridged)
driver/      AudioServerPlugIn HAL bundle + Xcode project
shared/      JackBridge.h — the IPC contract header, jb_log.hpp
installer/   build-pkg.sh, LaunchAgents, helper binaries, postinstall
pi/          systemd service + helpers for the pi-stomp side
tools/       chkshm / rmshm shm utilities, jackbridge-ctl
docs/        Architecture, setup, idiosyncrasies, spike results
```

Source of truth lives in `daemon/`, `driver/JackBridge/Plug-In/`, and `shared/`.

## Build

Both driver and daemon are targets in `driver/JackBridgePlugIn.xcodeproj`. The full release pipeline:

```bash
./installer/build-pkg.sh [version]      # xcodebuild → pkgbuild → productbuild
# Apple Silicon Homebrew:
JACK_PREFIX=/opt/homebrew ./installer/build-pkg.sh
```

Signing/notarization gates on `SIGN_APP_IDENTITY`, `SIGN_INSTALLER_IDENTITY`, `NOTARY_PROFILE` env vars; unset = unsigned local build.

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

- **IPC contract header** lives at `shared/JackBridge.h`. Both targets pick it up via `HEADER_SEARCH_PATHS=$(SRCROOT)/../shared`. Bump `JACKBRIDGE_PROTOCOL_VERSION` on every shm layout change — the refuse-on-mismatch handshake will then force a clean rebuild on both sides.
- **shm sync uses `std::atomic<uint64_t>` with explicit acquire/release.** `static_assert`s in `shared/JackBridge.h` pin size, alignment, and `is_always_lock_free`. Don't reintroduce `volatile`-as-synchronization.
- **Hardcoded `*2` and `8`-byte-per-frame literals** throughout assume stereo float per ring. Don't generalize without auditing every site.
- **`tools/rmshm.c` also unlinks legacy `/jackrouter` + `/jackrouter2` names** — intentional, helps users migrating from the upstream `madhatter68/JackRouter` install.
- **HAL flips `kAudioDevicePropertyDeviceIsAlive=0`** when the daemon's heartbeat (`shmDaemonAlive`) stalls past 5 ring-buffer cycles, then re-arms on `_HW_StartIO`. Don't paper over this with stale-buffer playback.

Full list with file/line citations: `docs/idiosyncrasies.md`.

## Development principles for this fork

- **Same-clock-domain assumption is load-bearing.** Mac jackd must run with the CoreAudio backend pinned to a stable hardware device; netJACK2 runs as an internal JACK client and handles the Pi↔Mac clock crossing itself. Do not add SRC to JackBridge — that's netJACK2's job. See `docs/architecture.md`.
- **Realtime safety in audio paths.** No allocation, no syscalls, no logging, no locks in the HAL IO proc or the daemon's JACK process callback. Ring-buffer memcpy only.
- **Apple Silicon first.** Test on arm64 hardware, not just under Rosetta. Universal binaries are produced but arm64 is the primary target.
- **Fail loud, not silent.** Refuse to attach on protocol mismatch; refuse to run on the wrong jackd backend; exit on bad `jack_client_open`. LaunchAgent `KeepAlive` + `WatchPaths` on `config.plist` handle restart.
- **Pragmatic over perfect.** The shm IPC layout, single-device assumption, and 4-in/2-out scope are *fine for the use case* — don't grow them speculatively.

## Logging

Both targets route through `shared/jb_log.hpp` → `os_log`, subsystem `com.jackbridge`, categories `daemon` / `driver` / `shm` / `jack`. Tail with:

```sh
log stream --predicate 'subsystem == "com.jackbridge"'
```

Format-string literals only; use `%{public}s` when caller-supplied strings need to be visible.

## Codesigning + macOS specifics

Driver and daemon both need hardened runtime; release builds use Developer ID + notarization inside a `.pkg`. Daemon carries `com.apple.security.cs.disable-library-validation` (in `daemon/daemon.entitlements`) to `dlopen` libjack. Install path is `/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`; daemon + helpers live under `/Library/Application Support/JackBridge/`. Postinstall does `killall coreaudiod` and `launchctl bootstrap` into the active GUI session.

Details: `docs/macos-setup.md`.
