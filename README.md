# JackBridge (former "JackRouter")
## About
This is an alternative to jackrouter for MacOS. JackBridge acts as a virtual
audio interface (currently 2in-4out) connected to Jackaudio server directly.
Even though the master clock of JackBridge becomes synchronized with Jack 
server, Core Audio Applications connected via JackBridge is out of jackaudio
connection graph scope. Therefore, I changed the name from "Router" to "Bridge".

NOTE: This is still experimental prototype implementation. Please be careful using it.

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
