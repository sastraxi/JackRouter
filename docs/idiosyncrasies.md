# Idiosyncrasies & gotchas

The things that will surprise you. Each item has a file:line citation where applicable.

## Repo / build

### `JackBridge.h` is byte-duplicated
`daemon/JackBridge.h` and `driver/JackBridge/Plug-In/JackBridge.h` are identical copies of the IPC contract. Edit one without the other and you silently corrupt shm. Phase 3 dedupes via Xcode workspace.

### `tools/rmshm.c` unlinks the wrong shm name
Targets `/jackrouter` (the old project name). Current daemon uses `/JackBridge`. Update before relying on it.

### `build.sh` is two lines
The daemon "build system" is a `g++` shell script. No deps tracking, no debug/release split, no codesigning. Phase 1 replaces with an Xcode target.

### x86_64-only Xcode project
`ARCHS = $(ARCHS_STANDARD_64_BIT)` with no arm64 in `VALID_ARCHS`. No `MACOSX_DEPLOYMENT_TARGET` set anywhere. Builds under Rosetta but `coreaudiod` on Apple Silicon **silently rejects non-arm64 HAL plug-ins** — no error, just doesn't appear in Audio MIDI Setup. Phase 1 makes it universal.

## IPC / synchronization

### No atomics, no memory barriers on shm
The shm sync fields are `volatile uint64_t*` and updated with bare pointer writes. Two FIXMEs in `daemon/JackBridge.cpp` admit this:
- L125 `// FIXME: Should be atomic operation and do memory barrier`
- L171, L184 `// FIXME: should be consider buffer overwrapping`

On x86 the strong memory model hides the bug. On Apple Silicon you get torn reads, stalled cycles, or worse. Phase 2 replaces with `std::atomic` + `memory_order_acquire`/`release`.

### `jack_on_shutdown` is registered as a comment
`daemon/jackClient.cpp:132` has the shutdown handler registration commented out. Consequence: if `jackd` exits, the daemon does not notice. `*shmDriverStatus` stays `STARTED`. The HAL keeps thinking the driver is alive. The DAW gets silence forever, no error, until the user kills and restarts the daemon manually. **This is the bug.** Phase 2 fixes it + flips `kAudioDevicePropertyDeviceIsAlive=0` on shutdown so DAWs disconnect cleanly.

### `JackClient::JackClient` fails silently
`daemon/jackClient.cpp:70` returns without logging if `jack_client_open` fails (error log is commented out). Caller doesn't check. Subsequent code dereferences `client` → crash. Phase 1 makes this loud.

### Daemon `main()` is `while(1) sleep(600);`
No signal handling. No shm cleanup on exit. Killing the daemon leaves a stale shm region; relaunching attaches to it without checking ownership or version. Phase 2 adds `sigwait` on SIGINT/SIGTERM, version-stamp the shm, refuse to attach on mismatch.

### `tools/rmshm.c` exists because of all of the above
Manual cleanup when the daemon dies dirty. That's why it's there. Fix the lifecycle, retire the tool.

## Channel count, sample rate, format

### "Streams" are stereo pairs, not channels
`JackBridge.h:67–70`: `NUM_INPUT_STREAMS=1`, `NUM_OUTPUT_STREAMS=2`, `MAX_STREAMS=2`, `MAX_CHANNELS = MAX_STREAMS * 2 = 4`. The device is 2-in / 4-out, organized as 1 input stream and 2 output streams, each stereo. Channel count appears mostly as `streams * 2`.

### Hardcoded `* 2` and `8` bytes per frame
Throughout `daemon/JackBridge.cpp` and `driver/JackBridge/Plug-In/SA_Device.cpp`, frame addressing assumes stereo float32. SA_Device.cpp has a comment near L1442 saying "byte sizes here assume a 16 bit stereo sample format" — the comment is wrong; the code is 32-bit float stereo. Bumping `MAX_STREAMS` alone works; non-stereo streams require auditing every `*2` and `8` literal.

### Sample rate is hardcoded in 6 places
`SA_Device.cpp:80` default, L841–847 and L1179–1201 advertisement lists, L919 and L1273 validators that *throw* on anything outside `{44100, 48000}`. The daemon does not enforce — it asks JACK what rate to run at. The HAL is the gate. Phase 2+ can add 88.2/96 if needed; not in scope for the initial revival.

### `STRBUFSZ=32KB` = 4096 stereo float frames
At 48k/256-buffer that's ~21 ms of headroom. Plenty under Config B (same clock domain). If anyone bumps to 96k or larger buffer, recompute.

## Clock sync

### The original "clock sync" assumes lockstep
`SA_Device.cpp:1340–1366` `GetZeroTimeStamp` uses the daemon's `zeroHostTime` directly. Daemon stamps it from `mach_absolute_time()` every `FramesPerBuffer` (L124–130 of `JackBridge.cpp`). This works **only if JACK and CoreAudio advance their sample clocks in lockstep** — i.e., the jackd that the daemon is connected to is itself driven by CoreAudio. See `architecture.md` "Config B."

If someone runs the daemon against a `jackd -d net` setup, this silently drifts. There is no resampler. There is no PI controller. The `check_progress` function at `JackBridge.cpp:341` *detects* the miss-sync and prints a warning — but does nothing to correct it.

The fix is documentation + a startup check, not SRC. **JackBridge should refuse to run if jackd isn't on a CoreAudio backend.** Phase 2 adds the check.

### `FramesPerBuffer = STRBUFNUM / 2` regardless of JACK's actual buffer size
`JackBridge.cpp` sets the timestamp cadence independent of the negotiated JACK period size. Works when JACK buffer divides evenly into the ring; subtly wrong otherwise. Audit during Phase 2.

## CoreAudio / HAL

### `CAMutex` held in the realtime IO callback
`SA_Device.cpp:1420, 1450` `ReadInputData` / `WriteOutputData` acquire `mIOMutex` in the audio thread. Technically a priority-inversion hazard. The actual work under the lock is just `memcpy`, so contention is rare and bounded. Apple's own `SimpleAudio` sample does the same thing — left as-is for now.

### Audio paths must remain allocation-free
Both the HAL IO ops and the daemon's JACK process callback must not allocate, syscall, log, or take heavyweight locks. The current code mostly obeys this. Don't break it when refactoring.

### Bundle path is fixed
`/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`. After installing, `sudo killall coreaudiod` to register. Apple Silicon: must be arm64 (or universal). Must be signed + notarized for Sequoia/Tahoe to load it under SIP.

## Aggregate device pitfall

If a user creates an aggregate device that **includes JackBridge** as a sub-device and points jackd at that aggregate, CoreAudio does not detect the cycle. You get silence, hard mute, or runaway depending on buffer ordering — never a clean error. Defensive check in the daemon (Phase 2): enumerate the aggregate jackd is bound to via `kAudioAggregateDevicePropertyActiveSubDeviceList` and refuse to start if our UID is in it.

## jackd on macOS

### Default realtime priority is 10
`jackd -R` on macOS starts at priority 10 unless `-P N` is given explicitly. The Pi-side default (running as a service) is 75. With the master at 10 and any browser / DAW / coreaudiod work going on, the netJACK2 master client misses deadlines constantly and slaves disconnect. `jackd-launch` must pass `-P 75` (or higher). See `spike-b-clock-stability.md`.

### `jackd -d coreaudio -d "<friendly name>"` is silently ignored
The user-visible device name (e.g. `"Steinberg UR22C"`, the same string Audio MIDI Setup shows) does not select the device. jackd falls back to "default input + default output" and, if those differ, auto-creates a cross-clock aggregate with a `clock drift compensation would be needed` warning — which is exactly the cross-clock topology Config B forbids. Use the internal CoreAudio name from `jackd -d coreaudio -l` instead (e.g. `AppleUSBAudioEngine:Yamaha Corporation:Steinberg UR22C:120000:1,2`). For the production aggregate, this is `~:<aggregate-uid>` as documented in `macos-setup.md`.

### `jackd -d coreaudio` does **not** take exclusive control of the device
Even while jackd is bound to a CoreAudio device, that device remains available as a system output — apps can play through it and the audio mixes with whatever's going through jackd. Useful (system audio keeps working during development) but a trap: **if the user sets the same device as both jackd's backend and the system output, they get a feedback loop / mix-of-everything** with no clear error. The aggregate-device strategy (built-in output as the aggregate's sub-device) avoids this for production, since the user is unlikely to pick the aggregate as their normal system output. Pass `-H`/`--hog` to force exclusive access if needed; we don't, to allow side-by-side dev workflows.

## netJACK2 slave reconnection — stale master entries

When a netJACK2 slave disconnects (e.g. `pi-stomp-jackbridge.service` restart), the Mac's netmanager (`JackNetMasterManager`) does **not** immediately clean up the stale `JackNetMaster` entry. Cleanup only happens when the master's socket error path sends a `KILL_MASTER` multicast packet, which the netmanager's `Run()` loop processes. If the slave reconnects before the Mac detects the socket error, `jack_client_open("pistomp")` fails because a JACK client with that name still exists — JACK appends `-01`, creating a duplicate slave that fights the original for the same network stream. This produces `WriteResample` / `ringbuffer failure` errors on the pi side and XRuns on both sides.

**Mitigation in `pi/bin/jackbridge-pi-up:57-64`:** A 3-second `sleep` (line 64) between `jack_unload` (line 53) and `jack_load` (line 70) gives the Mac time to detect the socket error and clean up the stale entry. This is a timing workaround — the jack2 fork has no name-based deduplication or timeout-based cleanup for stale slaves. See `common/JackNetManager.cpp:865-913` (`InitMaster`), `common/JackNetManager.cpp:928-943` (`KillMaster`), and `common/JackNetManager.cpp:915-926` (`FindMaster` — searches by ID only, not name).

## Proxy-ARP poisoning on link-local unicast

Even with the netJACK2 multicast group correctly pinned to the wired interface via `JACK_NETJACK_MULTICAST_IF`, the Mac's kernel sends ARP requests for the pi's link-local IP out **every** interface matching `169.254/16` (en7 + en0). The wifi router proxy-ARPs a reply with its own MAC on en0, which often arrives before the pi's real reply on en7, poisoning the cache. The fix is an interface-scoped host route (`route add -host <pi-ip> -interface <iface>`) that restricts ARP resolution to the wired interface. Since multicast packets don't trigger ARP learning on XNU, the `jackbridge-route-watcher` LaunchDaemon runs a background `tcpdump` loop on the wired interface to capture the pi's netJACK2 discovery (UDP to `225.3.19.154:19000`), extracts the source IP, and pins the host route. See `installer/jackbridge-route-watcher:101-131`.

**macOS `ping -I` caveat:** `ping -I <iface>` does not bind the ARP request to the interface on macOS — it uses `-b <iface>` instead. The route-watcher originally used `ping -I` which silently failed to pin ARP. Fixed to use `route add -host -interface`, which is persistent across ARP flushes and survives until the interface goes down.

## Naming

The repo is called `JackRouter` but the code is called `JackBridge`. The README explains this — the previous name was "JackRouter" (an actual CoreAudio router that lived in the JACK graph), and the rename reflects that the current design is "out of JACK graph scope" — CoreAudio apps connect via a bridge device, not as JACK ports. Don't be confused by the inconsistency.

The `SA_` prefix in `SA_Device.cpp` / `SA_PlugIn.cpp` is from Apple's "SimpleAudio" sample that this forked from.
