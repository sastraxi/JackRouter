# JackBridge revival plan

Production-ready macOS JACK ↔ CoreAudio bridge, targeting Sequoia 15.x / Tahoe 26.x on Apple Silicon, in service of the pi-stomp use case: a Raspberry Pi running JACK + netJACK2 as a network audio interface for macOS DAWs.

This plan assumes the architectural decisions in `docs/architecture.md`:
- Fork of `madhatter68/JackRouter` (not `jackaudio/jack-router`).
- Two-process design (daemon + AudioServerPlugIn HAL) via POSIX shm.
- **Config B clock topology**: jackd on Mac uses CoreAudio backend (aggregate device as clock source); netJACK2 runs as a JACK client inside jackd and handles Pi↔Mac clock crossing itself. JackBridge sees one clock domain. **No SRC in JackBridge.**
- Scope: 2-in / 4-out @ 48 kHz. Stereo wet return + stereo DI send is exactly the pi-stomp use case.

## Spikes (do first, ~1 day total)

Two unknowns can derail estimates if not de-risked early. Both are short.

### Spike A — sign + load a hello-world AudioServerPlugIn (½ day)
Build the simplest possible AudioServerPlugIn (BlackHole `develop` as a starting point), sign with Developer ID Application + hardened runtime, install to `/Library/Audio/Plug-Ins/HAL/`, `killall coreaudiod`, confirm it appears in Audio MIDI Setup on a clean Sequoia and Tahoe machine. De-risks the entire signing/loading pipeline before touching JackBridge code.

**Pass criteria:** device visible in Audio MIDI Setup on both Sequoia and Tahoe, Apple Silicon, with no Console errors from `coreaudiod`.

### Spike B — confirm Config B clock stability (½ day)
On a real Pi + Mac + Ethernet rig, run `jackd -d coreaudio` on the Mac with `jack_load netmanager`, Pi as netJACK2 slave. Run a sine generator on the Pi side, capture an hour into a Mac DAW, look for clicks / phase discontinuities. **The whole "no SRC needed" claim rides on this.** If it clicks, fall back to a SRC-in-daemon Phase 2 (adds ~5 days).

**Pass criteria:** zero clicks in 1 hour of capture. RMS-level continuity across the run.

---

## Phase 1 — Build + load on current macOS (4–6 days)

**Goal:** universal binary, signed, notarized, loads on Sequoia/Tahoe Apple Silicon. Existing (buggy) sync code unchanged. End-to-end audio passes, even if it eventually drifts.

### 1.1 Xcode project modernization (1 day)
- `driver/JackBridgePlugIn.xcodeproj/project.pbxproj`:
  - `ARCHS = arm64 x86_64`
  - `MACOSX_DEPLOYMENT_TARGET = 13.0`
  - `ONLY_ACTIVE_ARCH = NO` for Release
  - Modern Clang/C++17
- Audit `driver/JackBridge/PublicUtility/` — drop headers that overlap with current public SDK, keep `CAException`/`CADebugMacros`/`CAMutex` if still needed.
- Build a universal `.driver` bundle.

### 1.2 Daemon Xcode target (1 day)
- Delete `daemon/build.sh`. Add a CLI tool target inside the Xcode workspace.
- Universal binary. Link against JACK2 v1.9.22+ official `.pkg` at `/usr/local/lib/libjack.0.dylib`.
- Output: `JackBridged` binary, ready to sign.

### 1.3 Codesigning + entitlements (1 day)
- Driver: Developer ID Application, hardened runtime, no entitlements.
- Daemon: Developer ID Application, hardened runtime, entitlement `com.apple.security.cs.disable-library-validation` (for libjack).
- Generate `daemon.entitlements` plist. Document the cert setup in `docs/macos-setup.md` (already drafted).

### 1.4 Installer pipeline (1 day)
- `pkgbuild` → `productbuild` → `notarytool submit --wait` → `stapler staple`.
- Layout: driver to `/Library/Audio/Plug-Ins/HAL/`, daemon to `/Library/Application Support/JackBridge/`, LaunchAgents to `/Library/LaunchAgents/`.
- Postinstall script: `killall coreaudiod`, register LaunchAgents.

### 1.5 LaunchAgent plists (½ day)
- `com.jackbridge.daemon.plist` — runs the daemon, `LimitLoadToSessionType=Aqua`, `KeepAlive`.
- `com.jackbridge.jackd.plist` — runs the `jackd-launch` wrapper.
- Both per-user, instantiated per session.

### 1.6 Smoke test (½ day)
- Clean macOS install. Install `.pkg`. Reboot. Confirm:
  - JackBridge appears in Audio MIDI Setup.
  - Daemon + jackd running in `launchctl list`.
  - REAPER selects JackBridge as device, records 30 sec of audio without immediate failure.

**Phase 1 done when:** end-to-end audio works on Apple Silicon Sequoia/Tahoe. Known to drift over time / break on jackd restart — that's Phase 2.

---

## Phase 2 — Apple Silicon correctness + lifecycle (5–7 days)

**Goal:** no torn shm reads on arm64, clean recovery from jackd lifecycle events, defensive checks against misconfiguration.

### 2.1 Deduplicate `JackBridge.h` (½ day)
- Move IPC contract to a single header shared by both targets via Xcode workspace + shared header search path.
- Add `protocolVersion` constant. Bump on every layout change.

### 2.2 Replace `volatile` with `std::atomic` (1 day)
- All shm sync fields: `std::atomic<uint64_t>` for frame counters, `std::atomic<uint32_t>` for status flags.
- Publish with `memory_order_release`, read with `memory_order_acquire`.
- Verify on Apple Silicon — write a stress test that bangs the indices from both processes and checks invariants.

### 2.3 Heartbeat + version stamp (½ day)
- Daemon increments `daemonAlive` once per JACK process callback.
- HAL checks staleness in its IO proc (compare against host time at last update). If stale > 5 cycles, return silence and set `kAudioDevicePropertyDeviceIsAlive = 0`.
- Both sides refuse to attach if `protocolVersion` mismatches. Loud log, clean exit.

### 2.4 `jack_on_shutdown` + signal handling (1 day)
- Uncomment / re-implement `jack_on_shutdown` registration in `daemon/jackClient.cpp:132`.
- On shutdown: zero `daemonAlive`, mark shm dead, unlink, exit cleanly.
- Replace daemon `while(1) sleep(600);` with `sigwait` on SIGINT/SIGTERM. Clean teardown on signal.
- LaunchAgent `KeepAlive` brings it back automatically.

### 2.5 `jackd` backend sanity check (½ day)
- On daemon startup, query the jackd we connected to: confirm it's running on a CoreAudio backend (not `net`). If not, refuse to start with a clear error referencing `docs/macos-setup.md`.
- Implementation: `jack_get_driver_name()` or equivalent; if it's `net`, log and exit non-zero.

### 2.6 Aggregate-device feedback-loop check (½ day)
- Enumerate the aggregate jackd is bound to via `kAudioAggregateDevicePropertyActiveSubDeviceList`.
- If JackBridge's own UID is in the list, refuse to start. User has misconfigured. Loud log.

### 2.7 Loud failure on `jack_client_open` (½ day)
- Fix `daemon/jackClient.cpp:70` silent-return-on-failure. Log + exit non-zero.
- Audit all CoreAudio / JACK return codes — currently many are unchecked.

### 2.8 Soak test (1 day)
- 24-hour continuous run on real hardware (Pi → Mac, music playing through DAW). Zero clicks, zero hangs.
- Kill jackd mid-run, confirm DAW disconnects cleanly within one cycle.
- Restart jackd, confirm daemon reattaches without manual intervention.

**Phase 2 done when:** the system is robust to lifecycle disturbances and runs cleanly for a day on Apple Silicon.

---

## Phase 3 — Production polish (4–6 days)

**Goal:** shippable. End-user friendly install, sensible defaults, good diagnostics.

### 3.1 Aggregate device creation in installer (1 day)
- Postinstall script (or daemon first-run) calls `AudioHardwareCreateAggregateDevice` with `{built-in output}` as sub-device, named "JackBridge Clock," persists UID to `/Library/Application Support/JackBridge/aggregate-uid`.
- `jackd-launch` reads that UID, passes to `jackd -d coreaudio -d ~:<uid>`.
- Re-create on demand if the user deletes it from Audio MIDI Setup.

### 3.2 Meaningful channel labels (¼ day)
- Edit HAL property tables in `SA_Device.cpp` — channel names like "Pi Wet L/R", "Pi DI L/R" instead of generic "Input 1/2."
- Show up in DAW I/O pickers, makes the device self-explanatory.

### 3.3 Logging via `os_log` (½ day)
- Replace `printf`/`stderr` in daemon with `os_log` under subsystem `com.jackbridge`, categories `daemon`, `shm`, `jack`.
- HAL plugin uses `os_log` already (Apple convention). Confirm subsystem.
- Visible in Console.app and `log show --predicate 'subsystem == "com.jackbridge"'`.

### 3.4 Plist-based config (½ day)
- `/Library/Application Support/JackBridge/config.plist` for: jackd buffer size, sample rate (still 48k only), aggregate UID override, log level.
- Env-var overrides for debugging (`JACKBRIDGE_DEBUG`, `JACKBRIDGE_BUFFER`).

### 3.5 Delete dead code (¼ day)
- Remove `libs/` entirely.
- Remove `driver/ReadMe.txt` (Apple sample leftover).
- Update `tools/rmshm.c` to target `/JackBridge` (or delete — Phase 2 made it obsolete).
- Update README.md to reflect that there is no `JackBridge` branch.

### 3.6 Notarized installer pipeline in CI (1 day)
- GitHub Actions workflow: build → sign → notarize → staple → release.
- Secrets: Developer ID cert, app-specific password.
- Produces a `.pkg` per tagged release.

### 3.7 First-run TCC + permissions (½ day)
- One-shot helper app on first install to prompt for any required permissions (microphone TCC if needed for HAL input recording — verify).
- Documentation for the inevitable "I have to do what in System Settings?" question.

### 3.8 README + onboarding (½ day)
- Rewrite README.md targeting end users + downstream pi-stomp integrators.
- Quick-start: install `.pkg`, install JACK2, plug Ethernet to Pi, open DAW, pick JackBridge.
- Link into `docs/` for everything else.

**Phase 3 done when:** a user with no prior knowledge can install the `.pkg`, plug in a Pi, and have audio in their DAW within 5 minutes.

---

## Totals & risk

| Phase | Effort |
|---|---|
| Spikes | 1 day |
| Phase 1: build + load | 4–6 days |
| Phase 2: correctness | 5–7 days |
| Phase 3: polish | 4–6 days |
| **Total** | **14–20 person-days** |

### Riskiest unknowns

1. **`coreaudiod` plug-in validation on current macOS.** Spike A de-risks. If Apple has tightened HAL loading further (post-Tahoe), Phase 1 could blow out by a week.
2. **Config B clock stability.** Spike B de-risks. If clicks appear, Phase 2 grows by ~5 days to add SRC + PI controller in the daemon. Architecture is unchanged; cost is real but bounded.
3. **Notarization of a binary that `dlopen`s libjack.** The entitlement is documented and used by other projects (see BlackHole, BackgroundMusic), but notarization rejection is opaque. Submit a dummy `.pkg` early in Phase 1 to confirm the pipeline works.

### What this plan deliberately does not include

- **Higher channel counts.** Stays 2-in / 4-out. Out of scope for the pi-stomp use case; revisit only if asked.
- **Higher sample rates.** Stays 48 kHz. Same reasoning.
- **JACK-graph patchability** (i.e., making CoreAudio apps visible as JACK ports). The "out of JACK graph scope" property is fine here. Fixing it is a ~10-day rearchitecture for zero benefit in this topology.
- **Multi-instance support.** The commented-out `NUM_INSTANCES` scaffolding stays commented.
- **Windows / Linux.** This is the macOS bridge. Other platforms have working ASIO / native JACK paths.
- **GUI app / menu bar / preferences pane.** LaunchAgent + plist is sufficient. A GUI is post-1.0 nice-to-have, not required for the use case.

### Definition of done

A pi-stomp user can:
1. Download a notarized `JackBridge-x.y.z.pkg`.
2. Install it. Reboot.
3. Plug Ethernet from their Mac into the Pi.
4. Open Logic / Pro Tools / REAPER, select "JackBridge" as the audio device.
5. Hear audio from the Pi, send audio to the Pi.
6. Have it still work 24 hours later.
7. Have it survive a `jackd` crash and recover automatically.
