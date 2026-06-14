# JackBridge

A macOS JACK ↔ CoreAudio bridge: presents a virtual **JackBridge** audio device (4-in / 2-out @ 48 kHz) backed by a JACK client. 

The primary use case is a Raspberry Pi running netJACK2 over Ethernet as a recording interface for Mac DAWs (Logic, Pro Tools, REAPER).

---

## Daily Workflow (Ongoing Use)

It's easy to forget the order of operations. Follow these steps to get audio flowing:

1.  **Connect:** Plug the Ethernet cable directly from your Mac to the Pi.
2.  **Verify Mac Services:** Ensure the bridge services are running.
    ```sh
    jackbridge-ctl status
    # If not running:
    jackbridge-ctl start
    ```
3.  **Start Pi Service:** On the pi-Stomp device, go to the network menu, choose **"Wired Connection"**, then enable audio streaming.
4.  **Confirm Connection:** Run `jack_lsp | grep pistomp`. You should see several ports. If not, wait 10 seconds.
5.  **DAW Setup:** Open your DAW and select **JackBridge** as the audio device.

### What you get in the DAW

| DAW input  | Source                                            |
|------------|---------------------------------------------------|
| In1, In2   | Raw HW capture from pi (guitar pre-pedalboard)   |
| ModOut1/2  | Post-mod-host wet (the pedalboard tone)          |
| **Out1/2** | Stereo monitor return back to the pi             |

---

## When it doesn't work (Troubleshooting)

### 1. I lost Internet on my Mac!
macOS often prioritizes the Ethernet cable over Wi-Fi. Since the Pi has no internet gateway, your Mac gets "stuck" trying to use it.
*   **Fix:** System Settings > Network > ... (three dots) > **Set Service Order...** > Drag **Wi-Fi** above your Ethernet (sometimes "10/100/1000") device.

### 2. Services aren't starting (or I see the wrong ports)
*   **Conflicts:** If you have **MOD Desktop**, **Jamulus**, or **SONABUS** running, they might have started their own "default" JACK server. JackBridge will accidentally connect to theirs instead of its own managed one.
    *   **Fix:** Quit those apps, then run `jackbridge-ctl restart`.
*   **Check the logs:** `jackbridge-ctl logs`

### 3. Pi ports don't appear in `jack_lsp`
It's almost always the multicast route landing on the wrong interface (Wi-Fi instead of Ethernet).
```sh
# Force the route watcher to re-pin the interface
sudo launchctl kickstart -k system/com.jackbridge.route
```

### 4. Audio is silent but ports are visible
If `jack_lsp` shows `pistomp` ports but you hear nothing:
*   **Check Connections:** Run `jack_lsp -c`. Ensure the `pistomp` ports are actually connected to `JackBridge` ports. If not, check `AutoConnect` in your `config.plist`.
*   **Wait for Sync:** netJACK2's resampler can take 5–10 seconds to stabilize on a fresh connection.
*   **Pi-Side Check:** SSH into the Pi (`ssh pistomp@pistomp.local`) and run `jack_lsp`. If the Pi doesn't see its own `system` hardware ports, it has nothing to send to the Mac.
*   **Restart Pi-Stomp:** Sometimes the internal audio engine (`mod-host`) needs a kick. Toggle the "Ethernet Audio" setting off and on again.

### 5. Audio is distorted or has "clicks"
*   **XRuns:** Check the logs (`jackbridge-ctl logs`). If you see `JackEngine::XRun`, your latency settings are too aggressive.
*   **Fix:** Increase `JitterFrames` in `config.plist` (try 128 or 256).

---

## Installation

1. Install [JACK2](https://github.com/jackaudio/jack2-releases/releases) (1.9.22+). Required at runtime.
2. Download the latest `JackBridge-x.y.z.pkg` from Releases. Double-click and run.
3. Trust the unsigned package manually (Right-click > Open).

### Building Tools (Optional)
If you are working from source, compile the helper utilities:
```sh
gcc -O2 tools/rmshm.c -o tools/rmshm
gcc -O2 tools/chkshm.c -o tools/chkshm
```

For the pi side: install pistomp-arch with JackBridge enabled, plug Ethernet from Mac to pi, toggle "Ethernet Audio Interface" on the LCD.

## Configuration

`/Library/Application Support/JackBridge/config.plist` — saving it kicks the LaunchAgents (WatchPaths).

- `ClockDeviceUID` — CoreAudio UID for jackd's backend device. Empty = auto-detect built-in output.
- `PeriodFrames` — The dominant latency knob; 64 or 128 is recommended.
- `NetworkInterface` — Name of the NIC to use. Empty = auto-detect (prefers 169.254.x).

## Architecture & Building

Fork of [`madhatter68/JackRouter`](https://github.com/madhatter68/JackRouter), modernized for Apple Silicon and customized for pi-Stomp.

Two processes, one POSIX shm region (`/JackBridge`), atomic sync. The **driver** (HAL plugin) memcpys between the DAW and shm. The **daemon** (JACK client) memcpys between JACK and shm. No SRC inside JackBridge; clock-domain crossing is handled by netJACK2.

* [docs/architecture.md](docs/architecture.md) — Detailed design
* [docs/macos-setup.md](docs/macos-setup.md) — Edge cases
* [tools/jackbridge-ctl](tools/jackbridge-ctl) — Status/Stop/Start script
* [installer/build-pkg.sh](installer/build-pkg.sh) — Build the package

## License

See `LICENSE`. Inherits from the upstream `madhatter68/JackRouter` project.
