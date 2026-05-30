# JackBridge (former "JackRouter")
## About
This is an alternative to jackrouter for MacOS. JackBridge acts as a virtual
audio interface (currently 2in-4out) connected to Jackaudio server directly.
Even though the master clock of JackBridge becomes synchronized with Jack 
server, Core Audio Applications connected via JackBridge is out of jackaudio
connection graph scope. Therefore, I changed the name from "Router" to "Bridge".

NOTE: This is still experimental prototype implementation. Please be careful using it.

## Architecture

JackBridge consists of both a driver and a daemon, as we must connect to JACK from userland. They communicate via ringbuffers in a POSIX shared-memory region (`/JackBridge`).

- **Driver** — an `AudioServerPlugIn` HAL bundle loaded into `coreaudiod`. This is what makes JackBridge appear as a selectable audio device in DAWs. Its IO proc memcpys between the DAW's audio buffers and the shm rings.
- **Daemon** — a userland JACK client. Its process callback memcpys between the JACK graph and the shm rings.

Both sides run in the same CoreAudio host-clock domain, so no sample-rate conversion happens inside JackBridge. Clock-domain crossing (e.g. to a networked Pi) is netJACK2's job.

## Changes
- Master clock synchronization with Jack server

## Limitation
- Supports only 44.1/48kHz mode.

## Build
All JackBridge code lives on `master` — there is no `JackBridge` branch despite earlier docs to the contrary.

JackBridge consists of two parts, a daemon and a user-space Core Audio driver.

- JackBridge daemon

  ```
  cd daemon
  ./build.sh
  ```

  (`build.sh` is a 2-line `g++` invocation, to be replaced with an Xcode target in Phase 1.2 — see `PLAN.md`.)

- JackBridge driver

  Build the project named "JackBridgePlugIn.xcodeproj" with Xcode (or `xcodebuild -configuration Release`). Produces a universal arm64+x86_64 `.driver` bundle.

## Installation
- JackBridge daemon

  Locate wherever you like. Just execute after jackd.

- JackBridgePlugIn driver

  Copy all contents to '/Library/Audio/Plug-Ins/HAL' and restart coreaudiod.

```
sudo cp -r JackBridgePlugIn.driver /Library/Audio/Plug-Ins/HAL
sudo -u _coreaudiod killall coreaudiod
```

  Then you can see JackBridge device on your application. And you can
  also change configuration with Audio MIDI setup application.

## TODO
- Multi instance support

## Download
The pre-built binaries can be downloaded from http://linux-dtm.ivory.ne.jp/downloads/MacOS/JackBridge.zip
