# macOS setup

The user-facing runtime setup. What gets installed where, what processes run, how the pieces wire together.

## Components

| Component | Role | Lifecycle |
|---|---|---|
| `JackBridgePlugIn.driver` | AudioServerPlugIn HAL bundle. Presents the CoreAudio device DAWs select. | Loaded by `coreaudiod`. Reloads on `killall coreaudiod`. |
| `JackBridge` daemon | JACK client. Shuttles buffers between jackd and the HAL via shm. | LaunchAgent, per-user session. |
| `jackd` (from JACK2 official) | JACK audio server. Hosts the netJACK2 master client. | LaunchAgent, per-user session. |
| `jack-clock` aggregate device | Provides jackd's CoreAudio backend with a stable clock source. | Created at install time via `AudioHardwareCreateAggregateDevice`. |
| Pi-side `jackd -d net` | netJACK2 slave on the Raspberry Pi. | Pi systemd service. Out of scope for this doc. |

## Install paths

```
/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver    HAL bundle (system-wide)
/Library/Application Support/JackBridge/JackBridged    Daemon binary
/Library/LaunchAgents/com.jackbridge.daemon.plist      Daemon LaunchAgent
/Library/LaunchAgents/com.jackbridge.jackd.plist       jackd launcher
/Library/Application Support/JackBridge/jackd-launch   jackd wrapper script
~/Library/Logs/JackBridge/                              Unified-log mirror (debug)
```

LaunchAgents in `/Library/LaunchAgents/` are instantiated per-user-session. Each plist sets `LimitLoadToSessionType = Aqua` so they only run for GUI sessions (not SSH, not loginwindow). `coreaudiod` is per-session — system-wide LaunchDaemons (uid 0, no Aqua) cannot wire up to it cleanly, which is why we don't use them.

## Why an aggregate device?

`jackd -d coreaudio` requires a CoreAudio device to drive its cycle. We want this device to be:

1. **Always present** (so jackd starts reliably without depending on user hardware).
2. **Stable in clock** (so jackd's cycle doesn't jitter).
3. **Independent of whatever device the DAW selects** (no circular dependency).
4. **Independent of what JackBridge presents** (no feedback loop).

An aggregate device containing just the Mac's built-in output meets all four. The aggregate is created programmatically by the installer (or daemon's first-run helper) via `AudioHardwareCreateAggregateDevice` — no need to script Audio MIDI Setup. The aggregate's UID is persisted and passed to `jackd -d coreaudio -d ~:<uid>`.

**Do not include JackBridge in the aggregate.** See `idiosyncrasies.md` — CoreAudio does not detect the cycle; you get silence or runaway.

## Bringing the system up (post-install)

```bash
# Aggregate device created at install time, UID written to:
#   /Library/Application Support/JackBridge/aggregate-uid

# LaunchAgents start automatically at user login. Manual control:
launchctl load   ~/Library/LaunchAgents/com.jackbridge.jackd.plist
launchctl load   ~/Library/LaunchAgents/com.jackbridge.daemon.plist
launchctl unload ~/Library/LaunchAgents/com.jackbridge.daemon.plist
launchctl unload ~/Library/LaunchAgents/com.jackbridge.jackd.plist
```

The `jackd-launch` wrapper script invokes:
```
jackd -R -P 75 -d coreaudio -d ~:$(cat aggregate-uid) -r 48000 -p 128
```
then `jack_load netmanager` once jackd is responsive (netJACK2 master client).

**`-P 75` is required, not optional.** macOS jackd's default realtime priority is `10`, which loses to almost anything on the system; in testing, the netJACK2 master client missed its deadline ~2.3×/sec at the default and the slave connection actually dropped within 30s. `-P 75` (matching the Pi's typical setting) eliminates xruns. See `spike-b-clock-stability.md`.

## Selecting the device in a DAW

1. Install completes, `coreaudiod` is restarted.
2. DAW opens, audio device picker shows "JackBridge" alongside the user's other interfaces.
3. DAW selects JackBridge. 2 input channels, 4 output channels visible.
4. Pi-side jackd discovers the Mac's `netmanager` via multicast (default `225.3.19.154`) and the netJACK2 link comes up.
5. JACK graph routing happens on the Pi side (typically wired by pi-stomp or its host configuration).

## "System audio through the Pi" bonus mode

Setting **System Settings → Sound → Output → JackBridge** routes all system audio (Spotify, browser, video players, system alerts) through the chain → Pi → Pi's audio hardware. This is independent of the DAW use case.

Caveats:
- Latency ~5–15 ms end-to-end. Imperceptible for music, fine for video (players resync), edge-case for real-time interactive (Zoom, games).
- Stereo only. Multichannel system audio is summed/dropped.
- 48 kHz only. CoreAudio resamples app-side transparently.

## Codesigning + notarization

| Artifact | Signature | Hardened runtime | Entitlements | Notarized |
|---|---|---|---|---|
| `JackBridgePlugIn.driver` | Developer ID Application | Yes | None | Yes (inside `.pkg`) |
| `JackBridged` daemon | Developer ID Application | Yes | `com.apple.security.cs.disable-library-validation` (for `libjack.dylib`) | Yes (inside `.pkg`) |
| `JackBridge-x.y.z.pkg` | Developer ID Installer | n/a | n/a | Yes, stapled |

Sequence:
```bash
codesign --force --options runtime --sign "Developer ID Application: <Team>" \
    --entitlements daemon.entitlements JackBridged
codesign --force --options runtime --sign "Developer ID Application: <Team>" \
    --deep JackBridgePlugIn.driver

pkgbuild --root staging --identifier com.jackbridge.pkg --version 0.1.0 \
    --install-location / --scripts scripts JackBridge.pkg
productbuild --sign "Developer ID Installer: <Team>" \
    --package JackBridge.pkg JackBridge-signed.pkg

xcrun notarytool submit JackBridge-signed.pkg \
    --apple-id <id> --team-id <team> --password <app-specific> --wait
xcrun stapler staple JackBridge-signed.pkg
```

Without notarization the driver loads on the dev's machine but Gatekeeper blocks the `.pkg` for end users.

## libjack on the Mac

The daemon dynamic-links `libjack.0.dylib`. JACK2 v1.9.22+ ships a universal `.pkg` at https://github.com/jackaudio/jack2-releases/releases that installs to `/usr/local/lib/libjack.0.dylib`. **Do not use Homebrew's `jack` formula** — that's JACK1 and does not include JackRouter or netJACK2 in the same form.

The hardened-runtime entitlement `com.apple.security.cs.disable-library-validation` is required because libjack is signed by the JACK project, not by us, and library validation rejects cross-team-signed libraries by default.

## Troubleshooting

- **"JackBridge doesn't appear in Audio MIDI Setup"** — codesigning issue or non-arm64 build. Check `log show --predicate 'subsystem == "com.apple.audio.coreaudiod"' --last 10m` for rejection reasons.
- **"DAW sees JackBridge but no audio"** — daemon not running, or daemon running but jackd not. `launchctl list | grep jackbridge`.
- **"Audio works for 10 seconds then silence"** — jackd crashed. Without the (Phase 2) shutdown handler, JackBridge does not notice. `tail -f ~/Library/Logs/JackBridge/daemon.log`. Manual recovery: unload + reload both LaunchAgents.
- **"Clicks/dropouts"** — in order of likelihood: (1) Mac jackd missing `-P 75` (see above — default priority 10 underflows constantly); (2) Mac on Wi-Fi (netJACK2 needs wired gigabit on both ends); (3) Pi-side buffer too small — bump `jackd -p <period>`. Almost never JackBridge itself.
- **"Stale shm region"** — daemon died dirty. `tools/rmshm` (after fixing it to target `/JackBridge`), or just reboot.
