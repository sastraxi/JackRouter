# JackBridge — pi side

This directory is everything the Raspberry Pi needs to act as a 4-in / 2-out audio interface for a Mac DAW over Ethernet (netJACK2). On the Mac it shows up as the **JackBridge** CoreAudio device; on the pi it's a systemd unit that loads a netJACK2 *slave* into the already-running pi-stomp `jackd`.

Nothing here replaces the pi-stomp performance-pedal stack. `jack.service` + `mod-host` + `mod-ui` keep running. This service is a *second* JACK client, on-demand, controlled from the LCD.

If you just want to use the recording feature: open the LCD menu, find the new "Ethernet Audio Interface" entry, toggle it on, and pick `JackBridge` in your DAW. Everything below is for tweaking, debugging, or building from source.

## What you get

| DAW input  | Source on the pi                                    |
|------------|-----------------------------------------------------|
| In1, In2   | Raw hardware capture — guitar signal pre-pedalboard |
| ModOut1/2  | Post-mod-host wet feed — same audio as the headphone jack |

| DAW output | Goes to                                             |
|------------|-----------------------------------------------------|
| Out1, Out2 | Summed into `system:playback_*` alongside mod-host  |

In1/In2 are typical for *reamp* workflows (record dry, reprocess later). ModOut1/2 give you the wet pedalboard tone direct from the device.

## How it fits together

```
guitar → iqaudio capture ─┬─► mod-host wet → headphone jack
                          │                ↘
                          │                 netadapter:playback_3,4 (ModOut1,ModOut2)
                          └─► netadapter:playback_1,2  (In1, In2)
                                                       │
                                            netJACK2 UDP over Ethernet
                                                       │
                                                       ▼
                                              Mac netmanager → JackBridge HAL → DAW
```

The pi runs **netadapter** as an internal client of the stock `jackd` — it doesn't run its own jackd. That means the netadapter inherits jackd's sample rate, period size, and clock. **You can't tune those from this service.** They're whatever the pi-stomp image is configured to use (currently 48 kHz, 128 frames period — see `/lib/systemd/system/jack.service` for the truth).

## Installed layout

```
/usr/lib/systemd/system/pi-stomp-jackbridge.service     # the unit
/usr/local/libexec/jackbridge/jackbridge-pi-up          # ExecStart
/usr/local/libexec/jackbridge/jackbridge-pi-down        # ExecStop
/usr/local/libexec/jackbridge/jackbridge-xrun-watcher   # foreground process
/usr/local/libexec/jackbridge/jb-detect-net-iface       # auto-detect
/usr/local/libexec/jackbridge/jackbridge-pin-route      # ExecStartPre=+
/usr/local/libexec/jackbridge/jackbridge-unpin-route    # ExecStopPost=+
/etc/default/jackbridge                                 # optional env file (not created by default)
/tmp/pi-stomp-jackbridge.xruns                          # xrun log, runtime
/run/jackbridge.iface                                   # recorded iface, runtime
```

## Enable / disable

The LCD UI is the supported control path (a single Enable/Disable button). Everything below is the same flow from a shell:

```sh
sudo systemctl start  pi-stomp-jackbridge.service   # turn recording mode on
sudo systemctl stop   pi-stomp-jackbridge.service   # turn it off (default state)
systemctl is-active   pi-stomp-jackbridge.service
```

The service is intentionally **not** enabled at boot. The netadapter encode/decode is real CPU cost (~4 channels × 48k float → UDP) and isn't free for a performance pedal. Start it only when you want to record.

## Tunables

### Network interface (the only knob we own)

The service pins netJACK2's multicast group (`225.3.19.154`) to a specific NIC before loading netadapter. The detector picks, in order:

1. `$JACKBRIDGE_IFACE` if set and wired-with-carrier.
2. A wired iface with a `169.254.0.0/16` link-local address (direct Mac-to-pi cable).
3. Any other wired iface with carrier + IPv4.

**Wired only.** There is no Wi-Fi fallback — 4 channels of 48k over wireless will just produce mystery xruns. If no wired iface is up, the service start fails (loudly, in `journalctl`), which is the correct behavior.

To override, create `/etc/default/jackbridge`:

```sh
# /etc/default/jackbridge
JACKBRIDGE_IFACE=eth1
```

Then `sudo systemctl restart pi-stomp-jackbridge.service`. The systemd unit picks the file up via `EnvironmentFile=-/etc/default/jackbridge` (the `-` means "no error if missing").

### Sample rate / period / latency

These are **not** owned by this service. They come from the stock pi-stomp `jack.service` (`/lib/systemd/system/jack.service`). Defaults on a current pi-stomp image are 48 kHz / 128 frames / 2 periods. The netadapter inherits the rate and period directly. If you raise the period for headroom you raise it for the performance pedal too — that's how mod-host gets its audio.

If you really want to retune, edit the stock `jack.service`'s `ExecStart=` line and `systemctl daemon-reload && systemctl restart jack`. Then restart this service so netadapter picks up the new period.

End-to-end round-trip latency budget on direct Ethernet at 128/2/48k: see `docs/spike-c-latency-results.md` in the repo root. At defaults it's around 20ms roundtrip (I might have this wrong).

### Channel wiring

`jackbridge-pi-up` does the post-load `jack_connect`s. If you want a different topology (e.g. send the dry signal *somewhere else*, or take mod-host's `effect_29:out` directly to In3/In4), edit the script and `systemctl restart`. The script is intentionally short and grep-friendly.

There's a small bit of source-port name probing in there because mod-host's wet output naming varies across builds (`mod-monitor:out_*` vs `mod-host:monitor-out_*`). The first match wins; the candidates list is at the top of the `wire_wet` function.

## Logs and debugging

The service's own log:

```sh
sudo journalctl -u pi-stomp-jackbridge.service -f
```

JACK's xrun stream (what the watcher reads):

```sh
sudo journalctl -u jack -f | grep RingBuffer
```

xrun history file (one Unix-epoch timestamp per line, capped to the last 15 min):

```sh
cat /tmp/pi-stomp-jackbridge.xruns
```

This file is also what the LCD's "xrun 1m / 5m / 15m" counters read. It's truncated on every service start and atomically rewritten on every append, so it's safe to `cat` at any time.

Inspect current JACK graph (what netadapter loaded as):

```sh
JACK_PROMISCUOUS_SERVER=jack jack_lsp -c | grep -E 'netadapter|system'
```

Expected once the service is up: `netadapter:capture_{1,2}` (Mac → pi), `netadapter:playback_{1..4}` (pi → Mac), connected to the system + mod-monitor ports per the topology table above.

Check the pinned multicast route:

```sh
cat /run/jackbridge.iface     # which iface the route is pinned to
ip route get 225.3.19.154     # what the kernel actually thinks
```

## Building / installing from source

```sh
git clone https://github.com/sastraxi/JackRouter
sudo bash JackRouter/pi/install.sh
```

`install.sh` is idempotent — safe to re-run after pulling updates. It only stages files into `/usr/local/libexec/jackbridge/` and `/usr/lib/systemd/system/`. It does **not** enable the service (that's the LCD's job), so re-running won't change runtime state.

The pistomp-arch image build invokes this same script at image-bake time. The pi-stomp install isn't doing anything special — just running `install.sh`.

## What this README does not cover

- **How netJACK2 works on the wire.** See the upstream JACK2 docs.
- **The Mac side.** See the repo root README and `docs/macos-setup.md`.
- **The pi-stomp LCD code.** Lives in the pi-stomp repo; spec for the menu integration is at `../pi-stomp/JACKBRIDGE_RECORDING.md` in the JackRouter repo.
- **The IPC layout between the Mac daemon and HAL driver.** See `docs/architecture.md`. It's not relevant to the pi side.
