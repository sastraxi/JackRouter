# JackBridge revival plan

Production-ready macOS JACK ↔ CoreAudio bridge, targeting Sequoia 15.x / Tahoe 26.x on Apple Silicon, in service of the pi-stomp use case: a Raspberry Pi running JACK + netJACK2 as a network audio interface for macOS DAWs.

This plan assumes the architectural decisions in `docs/architecture.md`:
- Fork of `madhatter68/JackRouter` (not `jackaudio/jack-router`).
- Two-process design (daemon + AudioServerPlugIn HAL) via POSIX shm.
- **Config B clock topology**: jackd on Mac uses CoreAudio backend (aggregate device as clock source); netJACK2 runs as a JACK client inside jackd and handles Pi↔Mac clock crossing itself. JackBridge sees one clock domain. **No SRC in JackBridge.**
- Scope: 2-in / 4-out @ 48 kHz. Stereo wet return + stereo DI send is exactly the pi-stomp use case.

## Spikes (do first, ~1 day total)

Two unknowns can derail estimates if not de-risked early. Both are short.

### Spike A — sign + load a hello-world AudioServerPlugIn — **PASS (Sequoia)**
Done on Sequoia 15.7.2 / M1 Pro with ad-hoc sign. Tahoe + Developer ID deferred. See [docs/spike-a-hal-loading.md](docs/spike-a-hal-loading.md).

### Spike B — confirm Config B clock stability — **PARTIAL (30s smoke clean, 1hr deferred)**
30s smoke at 48k/1024-period clean once Mac jackd is at `-P 75` (default `-P 10` underflows constantly). Mac was on Wi-Fi, so the formal 1hr capture is deferred until wired-Ethernet is in place. See [docs/spike-b-clock-stability.md](docs/spike-b-clock-stability.md). Phase 1.5 must enforce `-P 75`.

### Spike C — characterize Config B round-trip latency — **TODO**
Same topology as Spike B (see that doc for details of how we set it up). Use `jack_iodelay` (impulse loopback) to measure round-trip latency Pi ↔ Mac across a small matrix: period sizes (128 / 256 / 512 / 1024 frames) × netjack2 cycles (1 / 2 / 3), all at 48 kHz. Produces a latency-vs-stability curve so we know which setting to default `jackd-launch` to in Phase 1.5 and what to advertise as the achievable floor for the pi-stomp use case. No formal pass criterion — the deliverable is a table + a recommended default. Spike doc to land at `docs/spike-c-latency.md` once run.

---

## Phase 1 — Build + load on current macOS (4–6 days)

**Goal:** universal binary, signed, notarized, loads on Sequoia/Tahoe Apple Silicon. Existing (buggy) sync code unchanged. End-to-end audio passes, even if it eventually drifts.

### 1.1 Xcode project modernization — **DONE**
Universal `.driver` (arm64 + x86_64) builds clean under Xcode 26.3 / SDK 26.2 with `MACOSX_DEPLOYMENT_TARGET=13.0`, `CLANG_CXX_LANGUAGE_STANDARD=c++17`, `ALWAYS_SEARCH_USER_PATHS=NO`, `ONLY_ACTIVE_ARCH=NO` for Release. PublicUtility headers all compile clean against the current SDK — no overlap conflicts to resolve; pruning unused headers (CAVolumeCurve, CAGuard, CAHostTimeBase) folds into Phase 3 dead-code deletion. Only outstanding warnings are 6 pre-existing sign-compare instances in `SA_Device.cpp`. Smoke-loading the built `.driver` is Phase 1.6.

### 1.2 Daemon Xcode target — **DONE**
`JackBridged` CLI tool target added to `driver/JackBridgePlugIn.xcodeproj` alongside the driver. Universal (arm64 + x86_64), links `/usr/local/lib/libjack.0.dylib` + CoreAudio/CoreFoundation/CoreMIDI, deployment target 13.0, c++17. `daemon/build.sh` deleted. Recovered `jackClient.{cpp,hpp}` from git (Phase 3.5 dead-code sweep had nuked `libs/` while symlinks in `daemon/` still pointed there); `daemon/JackBridge.h` stays a symlink to the driver copy until Phase 2.1 deduplicates the IPC header properly. Source-level signing entitlements are Phase 1.3.

### 1.3 Codesigning + entitlements — **DONE**
`ENABLE_HARDENED_RUNTIME=YES` set at the project level (applies to both targets). `daemon/daemon.entitlements` written with `com.apple.security.cs.disable-library-validation`; wired to the `JackBridged` target via `CODE_SIGN_ENTITLEMENTS`. Driver target carries no entitlements. Local builds ad-hoc-sign with `-o runtime` (verified: `codesign -dvv` shows `flags=adhoc,runtime`, daemon has the entitlement, driver has none). Developer ID identity is intentionally not baked in — the release pipeline (Phase 1.4 / 3.6) overrides `CODE_SIGN_IDENTITY` at `xcodebuild` time using the sequence already drafted in `docs/macos-setup.md`. Cert provisioning itself is the operator's job, not the project's.

### 1.4 Installer pipeline — **DONE (LaunchAgents deferred to 1.5)**
`installer/build-pkg.sh` orchestrates `xcodebuild` (both targets, Release) → staging → `pkgbuild` → `productbuild` → optional `notarytool submit --wait` + `stapler staple`. Distribution xml lives at `installer/distribution.xml.in` (versioned via `@VERSION@` substitution; pins min macOS 13.0, arm64+x86_64). Postinstall script is `installer/scripts/postinstall` — currently just `killall coreaudiod`; LaunchAgent registration lands in 1.5 alongside the plists themselves. Local smoke produces `installer/build/JackBridge-<ver>.pkg` with driver at `/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver` and daemon at `/Library/Application Support/JackBridge/JackBridged`; signing/notarization gate on `SIGN_APP_IDENTITY` / `SIGN_INSTALLER_IDENTITY` / `NOTARY_PROFILE` env vars so the same script serves dev and release. A dummy notarized submission (per the riskiest-unknowns note) still needs to be run once a Developer ID cert is in hand.

### 1.5 LaunchAgent plists — **DONE**
Two LaunchAgents under `installer/launchagents/`: `com.jackbridge.daemon.plist` runs `JackBridged`; `com.jackbridge.jackd.plist` runs `jackd-launch`. Both are `LimitLoadToSessionType=Aqua`, `KeepAlive`, `RunAtLoad`, `ProcessType=Interactive`; logs to `/tmp/<label>.{out,err}.log` for now (3.3 swaps in `os_log`). `installer/jackd-launch` wraps `jackd -R -P 75 -d coreaudio [-d ~:<aggregate-uid>] -r 48000 -p 128` then `jack_load netmanager` — reads `/Library/Application Support/JackBridge/aggregate-uid` when present, falls back to jackd's default device picker when not (3.1 writes the UID file at first-run). `build-pkg.sh` now stages the plists into `/Library/LaunchAgents/` and the wrapper alongside the daemon binary. Postinstall does the usual `killall coreaudiod` then bootstraps both labels into the active GUI user's session via `launchctl bootstrap gui/<uid>` so install-time activation doesn't require a logout cycle; the `Aqua` session-type constraint still gates auto-load for subsequent logins. `plutil -lint` clean on both plists.

### 1.6 Smoke test (½ day)
- Clean macOS install. Install `.pkg`. Reboot. Confirm:
  - JackBridge appears in Audio MIDI Setup.
  - Daemon + jackd running in `launchctl list`.
  - REAPER selects JackBridge as device, records 30 sec of audio without immediate failure.

**Phase 1 done when:** end-to-end audio works on Apple Silicon Sequoia/Tahoe. Known to drift over time / break on jackd restart — that's Phase 2.

---

## Phase 2 — Apple Silicon correctness + lifecycle (5–7 days)

**Goal:** no torn shm reads on arm64, clean recovery from jackd lifecycle events, defensive checks against misconfiguration.

### 2.1 Deduplicate `JackBridge.h` — **DONE**
Header moved to `shared/JackBridge.h` (git mv from `driver/JackBridge/Plug-In/JackBridge.h`; `daemon/JackBridge.h` symlink deleted). Both targets pick it up via `HEADER_SEARCH_PATHS=$(SRCROOT)/../shared` at the project level (daemon target uses `$(inherited)` to stack on top of `/usr/local/include`). Added `JACKBRIDGE_PROTOCOL_VERSION 1` — bump on every shm layout change; the refuse-on-mismatch handshake lands in 2.3. CLAUDE.md's "byte-duplicated header" note updated. No workspace was needed — both targets already live in the same `.xcodeproj`, and a workspace would just be ceremony around the single project. Both targets build clean against the new path.

### 2.2 Replace `volatile` with `std::atomic` — **DONE**
All shm sync fields in `JackBridgeDriverIF` now `std::atomic<uint64_t>*` (kept uint64_t storage throughout — preserves the on-disk layout, so the only contract change is sync semantics, not offsets/sizes). `attach_shm` uses `reinterpret_cast<std::atomic<uint64_t>*>` over the mmap'd region; three `static_assert`s in `shared/JackBridge.h` pin down size, alignment, and `is_always_lock_free` so a future toolchain regression breaks the build instead of silently corrupting the IPC. Daemon (~13 sites) and HAL (~11 sites) rewritten to explicit `->load(acquire)` / `->store(release)`; status-flag writes that previously assigned through both `mDriverStatus` and `*shmDriverStatus` are now split so the atomic store is unambiguous. `JACKBRIDGE_PROTOCOL_VERSION` bumped 1 → 2.

Stress test: `tools/stress_atomic.cpp` — universal binary, fork-based producer/consumer over POSIX shm. Two invariants checked: torn-read detection via `(i<<32)|i` mirrored values, and acquire-release pairing via a non-atomic buffer published behind a release-store seq. 5M iterations clean on both arm64 and x86_64 (10.5M / 17M consumer reads respectively, zero violations). Build line in the header — not an Xcode target, this is a one-shot verification utility.

The plan's recommendation of `uint32_t` for status flags was skipped: shrinking would force an offset re-layout for no measurable benefit on either target arch. Revisit if a future bump needs the four bytes.

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

### 3.2 Meaningful channel labels (¼ day) -- *SKIPPED*

### 3.3 Logging via `os_log` (½ day)
- Replace `printf`/`stderr` in daemon with `os_log` under subsystem `com.jackbridge`, categories `daemon`, `shm`, `jack`.
- HAL plugin uses `os_log` already (Apple convention). Confirm subsystem.
- Visible in Console.app and `log show --predicate 'subsystem == "com.jackbridge"'`.

### 3.4 Plist-based config (½ day)
- `/Library/Application Support/JackBridge/config.plist` for: jackd buffer size, sample rate (still 48k only), aggregate UID override, log level.
- Env-var overrides for debugging (`JACKBRIDGE_DEBUG`, `JACKBRIDGE_BUFFER`).

### 3.5 Delete dead code — **mostly DONE during Phase 1.1 sweep**
`libs/`, `driver/ReadMe.txt`, and the unused PublicUtility files (`CADebugger`, `CAGuard`, `CAVolumeCurve`) all deleted. README's `JackBridge` branch reference fixed. Remaining: update or delete `tools/rmshm.c` (still targets `/jackrouter`) — Phase 2's lifecycle fix likely makes it obsolete.

### 3.6 Notarized installer pipeline in CI (1 day)
- GitHub Actions workflow: build → sign → notarize → staple → release.
- Secrets: Developer ID cert, app-specific password.
- Produces a `.pkg` per tagged release.

### 3.7 First-run TCC + permissions (½ day)
- Installer that prompts for any required permissions (microphone TCC if needed for HAL input recording — verify).
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
