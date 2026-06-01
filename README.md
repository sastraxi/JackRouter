# JackBridge

A macOS JACK ↔ CoreAudio bridge: presents a virtual **JackBridge** audio device (4-in / 2-out @ 48 kHz) backed by a JACK client. The driving use case is a Raspberry Pi running netJACK2 over Ethernet as a recording interface for Mac DAWs (Logic, Pro Tools, REAPER) — see `pi/README.md` for that side. The Mac side works on its own too: anything that talks to your local `jackd` is reachable from any CoreAudio app.

Fork of [`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter), modernized for Apple Silicon and Sequoia/Tahoe. Roadmap: `PLAN.md`.

## Install

1. Install [JACK2](https://github.com/jackaudio/jack2-releases/releases) (1.9.22+). Required at runtime; we `dlopen` libjack.
2. Download the latest `JackBridge-x.y.z.pkg` from Releases. Double-click and run.
3. Open your DAW, pick **JackBridge** as the audio device. 4 inputs, 2 outputs.

The `.pkg` installs the HAL driver, the daemon, a managed `jackd` LaunchAgent (CoreAudio backend, `-P 75`, pinned to your built-in output for clock stability), and a system LaunchDaemon that pins the netJACK2 multicast route to the right NIC. Postinstall reloads `coreaudiod` and bootstraps everything into the current GUI session — no logout needed.

> Note: the install is not notarized or signed. You will have to trust it manually.

For the pi side: install pistomp-arch with JackBridge enabled, plug Ethernet from Mac to pi, toggle "Ethernet Audio Interface" on the LCD.

## What you get in the DAW

| DAW input  | Source                                            |
|------------|---------------------------------------------------|
| In1, In2   | Raw HW capture from pi (guitar pre-pedalboard)   |
| ModOut1/2  | Post-mod-host wet (the pedalboard tone)          |
| **Out1/2** | Stereo monitor return back to the pi             |

## When it doesn't work

```sh
sudo launchctl print system/com.jackbridge.route | grep -E 'state|pid'
cat /var/run/jackbridge-route.iface          # which NIC the route is pinned to
route -n get 225.3.19.154 | grep interface   # what the kernel thinks
jack_lsp | grep -i pistomp                   # pi attached?
log stream --predicate 'subsystem == "com.jackbridge"'
tail -f /tmp/com.jackbridge.{daemon,jackd}.err.log /var/log/com.jackbridge.route.log
```

If `pistomp:*` ports aren't visible after the pi service is running, it's almost always the multicast route landing on Wi-Fi. The route watcher should self-heal within a few seconds of a network-state change; force a recheck with `sudo launchctl kickstart -k system/com.jackbridge.route`.

If the DAW gets silence with the daemon and pi both up, version mismatch is the next thing to check: HAL and daemon must share the same `JACKBRIDGE_PROTOCOL_VERSION`. A stale driver after upgrade is the usual cause — `tools/rmshm` + reinstall the `.pkg`.

`docs/macos-setup.md` covers the rare cases (no built-in audio, headless Mac mini, etc).

## Configuration

`/Library/Application Support/JackBridge/config.plist` — saving it kicks the LaunchAgents (WatchPaths). Keys:

- `ClockDeviceUID` — CoreAudio UID for jackd's backend device. Empty = auto-detect built-in output. Set explicitly on headless Macs.
- `SampleRate`, `PeriodFrames`, `RealtimePriority` — jackd args. `RealtimePriority < 75` will warn (Spike B). `PeriodFrames` is the dominant latency knob; 128 ≈ 22 ms round-trip on direct Ethernet.
- `NetworkInterface` — name of the NIC to pin `225.3.19.154` to. Empty = auto-detect (direct-cable 169.254.x preferred, then any wired iface, then Wi-Fi).
- `NetJack:MTU` — bump to 9000 only if both ends and every switch support jumbo frames.
- `AutoConnect.{ToNetmanager, FromNetmanager, LocalMonitoring}` — internal wiring after daemon activate.

## Building from source

```sh
brew install jack                       # provides jackd + libjack
./installer/build-pkg.sh [version]      # one-shot: xcodebuild → pkgbuild → productbuild
```

Outputs `installer/build/JackBridge-<version>.pkg`. Unsigned by default; set `SIGN_APP_IDENTITY`, `SIGN_INSTALLER_IDENTITY`, `NOTARY_PROFILE` for a release build. Apple Silicon: `JACK_PREFIX=/opt/homebrew ./installer/build-pkg.sh`.

To work on the HAL or daemon directly, open `driver/JackBridgePlugIn.xcodeproj`. The daemon is a sibling target in the same project.

## Architecture (one paragraph)

Two processes, one POSIX shm region (`/JackBridge`), atomic acquire/release sync on Apple Silicon. The **driver** is an `AudioServerPlugIn` HAL bundle in `coreaudiod` — memcpys between the DAW's IO proc and the shm rings. The **daemon** is a userland JACK client — memcpys between its JACK process callback and the same rings. Both sides run in the same CoreAudio host-clock domain, so no SRC inside JackBridge. Clock-domain crossing to the pi is netJACK2's job. Heartbeat + protocol-version handshake flip `kAudioDevicePropertyDeviceIsAlive` off cleanly when jackd dies, so the DAW disconnects instead of getting forever-silence. Full tour in `docs/architecture.md`; gotchas in `docs/idiosyncrasies.md`.

## Pointers

- `pi/README.md` — pi side, end-user oriented
- `PLAN.md` — phased roadmap (Phases 1–4 done)
- `docs/architecture.md` — same-clock-domain design, shm layout, lifecycle
- `docs/macos-setup.md` — installation edge cases, manual uninstall
- `docs/idiosyncrasies.md` — quirks worth knowing before you patch
- `tools/` — `rmshm` (force-unlink the shm region), `chkshm` (inspect), `jackbridge-ctl` (status/stop/start)

## License

See `LICENSE`. Inherits from the upstream `madhatter68/JackRouter` project.
