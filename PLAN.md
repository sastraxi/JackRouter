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

### Spike C — characterize Config B round-trip latency — **DONE (Estimated)**
Same topology as Spike B. Link health verified bi-directionally on wired Ethernet via manual `jack_rec` tests. Due to `jack_iodelay` MLS loop instability (clock drift/resampling sensitivity), results were calculated using the NetJack2 formula calibrated against a successful 1024/2/48k baseline (4145 frames). 49-frame hardware overhead observed on Steinberg UR22C. Recommended default: 256/2 (~22ms). See [docs/spike-c-latency-results.md](docs/spike-c-latency-results.md) for the full matrix.

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

### 2.3 Heartbeat + version stamp — **DONE**
New shm fields at offsets 0x130 (`shmProtocolVersion`) and 0x138 (`shmDaemonAlive`); `JACKBRIDGE_PROTOCOL_VERSION` bumped 2 → 3. Version handshake centralized as `JackBridgeDriverIF::check_protocol_version()` — first-attacher writes, second validates; on mismatch both sides log loudly and exit (daemon `exit(1)`, HAL throws `kAudioHardwareBadDeviceError` from `_HW_Open`). Daemon `process_callback` ticks `shmDaemonAlive` with relaxed `fetch_add`. HAL tracks last-seen counter + host time in `mLastDaemonAlive` / `mLastDaemonAliveHostTime`; `GetZeroTimeStamp` flips `mDeviceIsAlive` (std::atomic<bool>) to false once `now - lastChange > 5 * HostTicksPerRingBuffer` and fires `Host_PropertiesChanged` for `kAudioDevicePropertyDeviceIsAlive` so the DAW disconnects rather than hanging on stale-buffer silence. The property getter now reads `mDeviceIsAlive` instead of hardcoded `1`; `ReadInputData` zeroes the destination when dead. Re-arm on `_HW_StartIO` so a daemon restart recovers without unloading the device. Built clean Debug both arches; runtime soak gates on hardware in 2.8.

### 2.4 `jack_on_shutdown` + signal handling — **DONE**
`JackClient` grew a virtual `on_shutdown()` hook + a `jack_on_shutdown(client, _on_shutdown, this)` registration in `activate()` (replacing the long-standing commented-out line at `jackClient.cpp:132`). `JackBridge::on_shutdown` zeros `shmDaemonAlive` and stamps `shmDriverStatus = INIT` so the HAL watchdog flips DeviceIsAlive immediately rather than waiting out the 5-cycle threshold, then `kill(getpid(), SIGTERM)` to wake `main()`. `main()` now does `pthread_sigmask(SIG_BLOCK, {SIGINT, SIGTERM})` before `activate()` so JACK's threads inherit the mask and the on_shutdown self-raise is steered to the main-thread `sigwait()` — that returns, we `delete jackBridge[0]` (which calls `jack_client_close`), and exit 0. We intentionally do NOT `shm_unlink` on the way out: the HAL is the shm owner, so unlinking would force a recreate cycle on next reconnect; the heartbeat-zeroing is the in-band liveness signal. LaunchAgent `KeepAlive` (1.5) restarts us when jackd is back. `tools/rmshm.c` is still the manual escape hatch — Phase 3.5 calls out updating it.

### 2.5 `jackd` backend sanity check (½ day)
- On daemon startup, query the jackd we connected to: confirm it's running on a CoreAudio backend (not `net`). If not, refuse to start with a clear error referencing `docs/macos-setup.md`.
- Implementation: `jack_get_driver_name()` or equivalent; if it's `net`, log and exit non-zero.

### 2.6 Aggregate-device feedback-loop check (½ day)
- Enumerate the aggregate jackd is bound to via `kAudioAggregateDevicePropertyActiveSubDeviceList`.
- If JackBridge's own UID is in the list, refuse to start. User has misconfigured. Loud log.

### 2.7 Loud failure on `jack_client_open` — **PARTIAL (silent-return fixed; broader audit deferred)**
`JackClient` ctor now logs the jackd status word + a pointer to `docs/macos-setup.md` and `exit(1)` instead of returning with `client == nullptr` and letting the next jack_* call segfault. The broader return-code audit across CoreAudio / JACK calls is still owed — folded into 2.5/2.6 for the jackd-side checks and otherwise into Phase 3 polish.

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

### 3.3 Logging via `os_log` — **DONE**
New `shared/jb_log.hpp` shim wraps `os_log_create("com.jackbridge", …)` with categories `daemon`, `driver`, `shm`, `jack` and `JB_LOG_{ERR,INFO,DEFAULT,DEBUG}` macros that pass format-string literals (so they aren't redacted as `<private>`; `%{public}s` is used where caller-supplied strings need to be visible). Plan claimed the HAL already used `os_log` — it didn't, it was on raw `syslog`. Both targets now route everything through the shim: daemon attach/shutdown/version/heartbeat paths, plug-in StaticInitializer/CreateDevices, device init/StartIO/StopIO, watchdog flip. Two known caveats: (1) the inside-RT verbose `printf`s inside `process_callback` / `check_progress` are left alone with a FIXME — pre-existing RT-safety violation, not 3.3's job to fix; (2) usage/help text on argv parsing still goes to `stderr` because that path is operator-CLI-invoked, not LaunchAgent-invoked. Tail with `log stream --predicate 'subsystem == "com.jackbridge"'`.

### 3.4 Plist-based config — **PARTIAL (defaults file + WatchPaths landed; parsing deferred)**
Default `config.plist` is staged to `/Library/Application Support/JackBridge/config.plist` by `installer/build-pkg.sh`; both LaunchAgents have `WatchPaths` pointing at it, so a save triggers `launchctl` to restart jackd + daemon automatically. Schema documented in-file. Currently informational only — no consumer reads it yet. Full wire-up is 3.4.x below.

### 3.4.1 Reader shim (½ day)
- `shared/jb_config.{hpp,cpp}` — single source of truth for parsing. C++ side (daemon) uses `CFPreferencesCopyAppValue` / `CFPropertyListCreateWithData` against the absolute path; shell side (`jackd-launch`) uses `/usr/libexec/PlistBuddy -c "Print :<key>"` with `2>/dev/null || echo <default>` fallbacks. Missing file = all defaults. Missing key = that key's default. Malformed file = log loudly, fall back to defaults, do **not** refuse to start (fail-loud-but-keep-going; an unreadable config shouldn't brick audio).
- Defaults live in code, not in the installed file. The installed file is purely a template the user can edit; deleting it must still produce a working system on next restart.

### 3.4.2 jackd-launch consumption (¼ day)
- Read `SampleRate`, `PeriodFrames`, `RealtimePriority`, `AggregateDeviceUID`, `NetJack:MTU` via PlistBuddy.
- `AggregateDeviceUID` from config takes precedence over the legacy `aggregate-uid` file (Phase 3.1 helper migrates the file's contents into the plist on first run, then deletes it).
- `RealtimePriority < 75` logs a warning citing Spike B before honoring it.
- MTU passes through to `jack_load netmanager -i "--mtu <n>"`.

### 3.4.3 Daemon consumption (½ day)
- `AutoConnect.{ToNetmanager,FromNetmanager,LocalMonitoring}` drive a post-`jack_activate` wiring pass: enumerate `netmanager:*` and `system:playback_*` ports, `jack_connect` per the policy. Reconnect on JackPortRegistration callbacks so connections survive netmanager (re)loads.
- `Logging.Level` maps to the `jb_log` shim's threshold (error/warn/info/debug → `OS_LOG_TYPE_*`).
- All reads happen once at startup. SIGHUP handler deferred — `WatchPaths`-triggered full restart is the reload mechanism and it's good enough; no incremental reload until profiling says otherwise.

### 3.4.4 Upgrade safety (¼ day)
- pkg currently overwrites `config.plist` on upgrade, which would clobber user edits. Move the file out of the component payload and have `postinstall` write it only if absent (`[ -f "$DEST" ] || install ...`). Bundle the default as a sibling `config.plist.default` for diffing.
- Migration: if `aggregate-uid` exists and the plist's `AggregateDeviceUID` is empty, fold the file contents into the plist and remove the file. Idempotent.

### 3.5 Delete dead code — **DONE**
`libs/`, `driver/ReadMe.txt`, and the unused PublicUtility files (`CADebugger`, `CAGuard`, `CAVolumeCurve`) all deleted. README's `JackBridge` branch reference fixed. `tools/rmshm.c` already targets `/JackBridge`; the additional `/jackrouter` + `/jackrouter2` unlinks are intentional upgrade cleanup for users coming from upstream `madhatter68/JackRouter` — CLAUDE.md updated to reflect that it's a feature, not vestigial.

### 3.6 Notarized installer pipeline in CI — **DONE (pending end-to-end run with real secrets)**
Two workflows under `.github/workflows/`:
- `ci.yml` — PR + master push smoke build. Installs JACK2 via Homebrew, shims `/opt/homebrew/{include,lib}/jack*` into `/usr/local/{include,lib}` so the daemon target's hardcoded paths still resolve on arm64 runners, then `xcodebuild` Release for both targets, both arches. No signing.
- `release.yml` — fires on `v*.*.*` tag push. Imports the Developer ID `.p12` (base64-encoded in `APPLE_DEVELOPER_ID_CERT_P12`) into a per-run keychain, `set-key-partition-list` to silence codesign prompts, `notarytool store-credentials` for a `jackbridge-notary` keychain profile, then `installer/build-pkg.sh "$version"` with `SIGN_APP_IDENTITY` / `SIGN_INSTALLER_IDENTITY` / `NOTARY_PROFILE` env vars. Keychain is torn down in an `if: always()` step. `softprops/action-gh-release@v2` attaches the `.pkg` to a generated release.
Required secrets are documented at the top of `release.yml`. The `/usr/local` shim was retired in the same pass: a project-level `JACK_PREFIX` build setting (default `/usr/local`) now stands in for the three hardcoded daemon-target paths plus the `libjack.0.dylib` file reference, so arm64 Homebrew installs work directly by passing `JACK_PREFIX=/opt/homebrew` (CI workflows do this from `brew --prefix`). `installer/build-pkg.sh` reads the same env var for its preflight check and forwards it to `xcodebuild`.

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
