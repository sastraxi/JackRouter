# Guide for JackBridge Development

JackBridge is a macOS JACK ↔ CoreAudio bridge. A user-space `AudioServerPlugIn` HAL driver presents a virtual audio device; a userland daemon shuttles buffers between JACK and the driver via POSIX shared memory.

The downstream use case driving this fork: a Raspberry Pi running JACK + netJACK2 as a network audio interface for macOS DAWs (Logic, Pro Tools, REAPER). See `PLAN.md` for the revival roadmap and `docs/` for everything else.

## Repo layout

```
daemon/    JACK client + shm publisher (CLI binary)
driver/    AudioServerPlugIn HAL bundle (Xcode project)
tools/     chkshm / rmshm shm-inspection utilities
docs/      Architecture, codebase tour, idiosyncrasies, setup
```

Source of truth lives in `daemon/` and `driver/JackBridge/Plug-In/`.

## Build

```bash
# Daemon (current state — to be replaced with Xcode/CMake target)
cd daemon && ./build.sh

# Driver
open driver/JackBridgePlugIn.xcodeproj   # build in Xcode

# Install driver
sudo cp -R build/JackBridgePlugIn.driver /Library/Audio/Plug-Ins/HAL/
sudo killall coreaudiod
```

The daemon's `build.sh` is a 2-line `g++` invocation. Replace with an Xcode target as part of Phase 1 modernization (see `PLAN.md`).

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

- **`JackBridge.h` is byte-duplicated** between `daemon/` and `driver/JackBridge/Plug-In/`. The IPC contract is maintained by hand. Phase 3 deduplicates this.
- **POSIX shm uses `volatile` reads, no atomics, no barriers.** Daemon's own FIXMEs admit it. Works on x86 by accident; broken on Apple Silicon.
- **Hardcoded `*2` and `8`-byte-per-frame literals** throughout assume stereo float. Don't generalize without auditing every site.
- **No `jack_on_shutdown` handler.** If jackd dies, HAL keeps reporting STARTED and DAW gets silence forever.
- **Daemon `main()` is `while(1) sleep(600);`** — no signal handling, no shm cleanup on exit. That's what `tools/rmshm.c` is for.
- **`tools/rmshm.c` still unlinks the old `/jackrouter` shm name** — leftover from the previous incarnation.

Full list with file/line citations: `docs/idiosyncrasies.md`.

## Development principles for this fork

- **Same-clock-domain assumption is load-bearing.** Mac jackd must run with the CoreAudio backend; netJACK2 runs as an internal JACK client and handles the Pi↔Mac clock crossing itself. Do not add SRC to JackBridge — that's netJACK2's job. See `docs/architecture.md`.
- **Realtime safety in audio paths.** No allocation, no syscalls, no logging, no locks in the HAL IO proc or the daemon's JACK process callback. Ring-buffer memcpy only.
- **Apple Silicon first.** All shm synchronization uses `std::atomic` with explicit `memory_order_acquire` / `release`. No `volatile`-as-synchronization. Test on arm64 hardware, not just under Rosetta.
- **Fail loud, not silent.** Heartbeat + version-stamp on the shm region. Refuse to attach on mismatch. Flip `kAudioDevicePropertyDeviceIsAlive=0` when daemon goes away — DAWs disconnect cleanly instead of getting forever-silence.
- **Pragmatic over perfect.** This is a prototype being hardened, not a clean-slate rewrite. The shm IPC layout, single-device assumption, and 2-in/4-out scope are *fine for the use case* — don't grow them speculatively.

## Codesigning + macOS specifics

Driver and daemon both need Developer ID signatures, hardened runtime, and notarization inside a `.pkg`. Daemon needs `com.apple.security.cs.disable-library-validation` to `dlopen` libjack. Install path stays `/Library/Audio/Plug-Ins/HAL/JackBridgePlugIn.driver`. Restart with `sudo killall coreaudiod` after install.

Details: `docs/macos-setup.md`.
