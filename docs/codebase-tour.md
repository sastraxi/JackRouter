# Codebase tour

A guided walk through the files that matter, what's in them, and what's safe to ignore.

## Top level

```
daemon/                  Source of truth for the userland process
driver/                  Source of truth for the HAL bundle (Xcode project)
libs/                    Stale earlier iteration — ignore (see "Dead code" below)
tools/                   Two tiny shm utilities
README.md                Stale — says `git checkout JackBridge` (branch doesn't exist)
LICENSE                  GPL (inherited from JACK)
```

## `daemon/` — the JACK-side process

```
JackBridge.cpp       421 LOC   Main: opens shm, drives JACK client, FIXMEs everywhere
JackBridge.h                   IPC contract (struct layout, sizing macros)
jackClient.cpp                 Wraps jack_client_open, port registration
jackClient.hpp                 Header for above
build.sh             2 lines   `g++ -std=c++11 ... -ljack -framework CoreAudio ...`
```

**`JackBridge.cpp` highlights:**
- L55 `isSyncMode = true; // FIXME: should be parameterized` — no runtime config; only `JACKBRIDGE_DEBUG` env var.
- L60 `*shmSyncMode = 0;` at startup → L114 `*shmSyncMode = 1;` after `isActive` flips. Clock sync engages here.
- L124–130 master clock stamping (`mach_absolute_time()` into shm every `FramesPerBuffer`). FIXME admits missing barrier.
- L171, L184 ring-buffer index advance — FIXME: "should be consider buffer overwrapping" (twice).
- L196 `malloc` of port name strings, never freed.
- L341 `check_progress` — detects miss-sync, only prints a warning.
- L409, L413 commented-out scaffolding for a second instance (`NUM_INSTANCES > 1`).
- `main()` ends with `while(1) sleep(600);`. No signal handling.

**`JackBridge.h` constants worth knowing:**
- L67–70: `NUM_INPUT_STREAMS=1`, `NUM_OUTPUT_STREAMS=2`, `MAX_STREAMS=2`, `MAX_CHANNELS=4`. Each "stream" is 2 channels.
- `STRBUFNUM=1024`, `STRBUFSZ=32768` (bytes per ring slice).
- `FRAMESPERBUFFER` derived. Implicit assumption: 8 bytes per frame (stereo float32).

**`jackClient.cpp` highlights:**
- L70 `jack_client_open` failure path returns silently (error log commented out). Subsequent code dereferences `client` → crash.
- L132 `jack_on_shutdown` registration is commented out. **This is the bug that makes jackd-died-silence permanent.**

## `driver/` — the AudioServerPlugIn HAL bundle

```
JackBridgePlugIn.xcodeproj/      Xcode project. x86_64 only, no MACOSX_DEPLOYMENT_TARGET.
JackBridge/Plug-In/
    SA_PlugIn.cpp                AudioServerPlugInDriverRef boilerplate
    SA_PlugIn.h
    SA_PlugIn-Info.plist         Bundle Info.plist (HAL driver identifier here)
    SA_Device.cpp     1577 LOC   The device: property tables, IO ops, all the meat
    SA_Device.h
    SA_Object.cpp                Base class for HAL objects
    SA_Object.h
    JackBridge.h                 BYTE-DUPLICATED copy of daemon/JackBridge.h
    Resources/                   Bundle resources
PublicUtility/                   Vendored 2012-era Apple CoreAudio sample utilities
ReadMe.txt                       Original SimpleAudio sample readme — ignore
```

**`SA_Device.cpp` highlights:**
- L80 `kSampleRate_TheItem = 48000.0` — default SR.
- L841–847, L1179–1201 `AvailableNominalSampleRates` lists — hardcoded `{44100, 48000}`.
- L919, L1273 SR validators throw on anything outside the list.
- L1154, L1185, L1198 `mChannelsPerFrame = 2` hardcoded.
- L1340–1366 `GetZeroTimeStamp` — reads daemon's `zeroHostTime` when `syncMode==1`. This is where clock sync lands on the HAL side.
- L1420, L1450 `ReadInputData` / `WriteOutputData` — the realtime hot path. Acquires `CAMutex mIOMutex` (Apple's SimpleAudio does the same; technically a priority-inversion hazard, but the work is just memcpy).
- L1442, L1445, L1472, L1475 ring-buffer memcpy with hardcoded `8` bytes/frame and `* 2` channel multipliers. Comment says "byte sizes here assume a 16 bit stereo sample format" — comment is wrong, it's 32-bit float stereo.

**`PublicUtility/`** is Apple's `CAException`, `CADebugMacros`, `CAMutex`, `CAAtomic` etc. from the `CoreAudioUtilityClasses` sample. Current SDKs ship some of these in public headers; Phase 1 should keep only what isn't in the SDK.

## `libs/` — DEAD CODE

```
audio.hpp        Older audio scaffolding — unused
coreAudio.hpp    Older CoreAudio glue — unused
jackClient.cpp   Earlier copy of the JACK client — superseded by daemon/jackClient.cpp
jackClient.hpp   Earlier header — superseded
midi.hpp         MIDI bridging code (was used by JackBridgeWithMidi variant)
```

This whole directory is from a previous iteration. The current daemon does not link any of it. Searching for symbols will return hits in both `libs/` and `daemon/` — always trust `daemon/`. Phase 3 deletes `libs/` entirely.

## `tools/`

```
chkshm.c    Read and dump the shm region for debugging. Useful for verifying daemon is alive.
rmshm.c     Unlink stale shm. STILL TARGETS THE OLD `/jackrouter` NAME — needs updating to `/JackBridge`.
```

Both are single-file C utilities. `rmshm` is the manual cleanup tool for the daemon's missing signal handler.

## What to read first

If you're new to the codebase, in order:
1. `daemon/JackBridge.h` — the IPC contract. Everything else is shaped by this.
2. `daemon/JackBridge.cpp` `main()` and the JACK process callback.
3. `driver/JackBridge/Plug-In/SA_Device.cpp` `BeginIOOperation` / `DoIOOperation`.
4. `driver/JackBridge/Plug-In/SA_Device.cpp` property tables (`HasProperty`, `GetPropertyData`).

That's ~30% of the code and ~95% of what matters.
