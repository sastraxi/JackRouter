# Move the netJACK2 slave to the Mac

**Status:** proposed, not yet implemented. Captures the topology flip we want
to try to push the adaptive resampler off the pi (weakest CPU) and onto the
Mac (lots of headroom + a well-behaved CoreAudio clock).

## Why

Pi journal over 30 minutes (empty pedalboard, `-g 256` pinning ring size):

```
589,878  "consumer too slow"   (ring filled, frames skipped)
276,288  "producer too slow"   (ring empty, frames missing)
  3,236  PushAndPull resets
```

Failures ramp monotonically from jackd start (e.g. 37 → 82 → 121 → 189 → 226
→ 272 per minute), which is the signature of clock drift integrating
linearly. The slip ring + `JackLibSampleRateResampler` on the pi can't track
the drift fast enough; pi `-g 256` pinned ring size, but a bigger ring just
delays overflow — it can't cancel a constant-sign drift rate.

The dominant direction (consumer-too-slow 2× producer-too-slow) suggests Mac
clock faster than pi ALSA hw:0 clock. Whichever direction, the resampler
needs to do real work. Moving it to the Mac:

- Apple Silicon vs Pi 5 CPU: net win for SRC quality knobs (`-q 32`+).
- CoreAudio's clock is steadier than pi ALSA — easier reference for the
  loop to lock to.
- pi-stomp's mod-host budget stops competing with the resampler for cycles.

## Current topology

```
Pi (slave)                        Mac (master)
─────────────                     ─────────────
jackd -d alsa -d hw:0             jackd -d coreaudio (CLOCK_UID)
  └─ jack_load netadapter           └─ jack_load netmanager
       -C 2 -P 4 -q 0 -g 256             (default opts)
     └─ slip ring + SRC             └─ exposes Mac slave for connect
```

Pi runs the resampler. Mac is the clock master.

## Target topology

```
Pi (master)                       Mac (slave)
─────────────                     ─────────────
jackd -d alsa -d hw:0             jackd -d coreaudio (CLOCK_UID)
  └─ jack_load netmanager           └─ jack_load netadapter
                                         -C 4 -P 2 -q 32 -g 256
                                       └─ slip ring + SRC
```

Mac runs the resampler. Pi ALSA hw:0 is the clock master. Channel counts
invert because `-C` / `-P` are slave-local (`-C` = FROM master, `-P` = TO
master): the Mac slave wants 4 capture (pi → Mac) and 2 playback (Mac → pi).

## Files that change

### 1. `pi/bin/jackbridge-pi-up` (pi-side loader)

Swap the internal client load:

```diff
- jack_load netadapter -i "-C 2 -P 4 -q 0 -g 256"
+ jack_load netmanager
```

Rewrite the `jack_connect` block. With netmanager on pi, the Mac slave shows
up as a JACK client (name = whatever Mac netadapter advertises, typically
hostname). Port-name semantics flip to master's POV:

- `<mac>:from_slave_{1..4}` = audio coming FROM Mac slave (4ch pi→Mac path
  — wait, no: this is what the slave sends TO master, which is the slave's
  `-P` direction = 2ch Mac→pi). Re-derive from the actual `jack_lsp` once
  it's up; the comment in the current pi-up file (lines 16–19) becomes wrong
  under netmanager.

Tentative wiring (verify with `jack_lsp` before committing):

```sh
# Mac → pi (2ch): from_slave_{1,2} → system:playback_{1,2}
jack_connect <mac>:from_slave_1 system:playback_1
jack_connect <mac>:from_slave_2 system:playback_2

# pi → Mac (4ch): system:capture + mod-host monitor → to_slave_{1..4}
jack_connect system:capture_1            <mac>:to_slave_1
jack_connect system:capture_2            <mac>:to_slave_2
jack_connect mod-monitor:out_1           <mac>:to_slave_3   # or mod-host:monitor-out_1
jack_connect mod-monitor:out_2           <mac>:to_slave_4
```

The `wire_wet` helper that picks the first available monitor source name can
stay; only the destination changes.

### 2. `installer/jackd-launch` (Mac-side loader, line ~98)

```diff
- "$JACK_LOAD" netmanager
+ "$JACK_LOAD" netadapter -i "-C 4 -P 2 -q 32 -g 256"
```

`-q 32` (vs pi's `-q 0`) buys higher resampler quality now that we have CPU
to spend. Keep `-g 256` for the pinned ring; tune later.

### 3. `daemon/JackBridge.cpp` (auto-wire, lines 342–354)

Current `auto_wire()` searches for `*:from_slave_N` / `*:to_slave_N`. On
Mac-as-slave those names don't exist — the local netadapter exposes
`netadapter:capture_N` (FROM master, 4ch in) and `netadapter:playback_N`
(TO master, 2ch out).

Two options:

**(a) Quick hack — additive second pass.** Keep existing calls, add:

```cpp
wire_direction("capture",  JackPortIsOutput, audioIn,  nAudioIn);
wire_direction("playback", JackPortIsInput,  audioOut, nAudioOut);
```

Also extend the port-registration callback (`_port_registration_callback`,
line 368) to trigger re-wire on `capture_`/`playback_` names as well as
`from_slave_`/`to_slave_`.

Pro: 4-line change, reversible, works whichever topology is active.
Con: ugly, two name schemes leaking into the daemon.

**(b) Clean — parameterize.** Add a config.plist key
`Topology = MacSlave | MacMaster` that selects which name pair to wire.
Rename `ToNetmanager` / `FromNetmanager` to `ToRemote` / `FromRemote`.

Pro: legible.
Con: bigger diff, more places to revert if the experiment fails.

Recommend **(a)** first, promote to **(b)** if the flip wins.

### 4. `installer/config.plist` (cosmetic, lines 75–88)

`ToNetmanager` / `FromNetmanager` key names + comments become stale.
Defer to after the experiment proves out.

## Risks / things that could go wrong

- **Pi as master loses sync to mod-host's existing wiring assumptions.** The
  `wire_wet` helper assumes mod-host is up by the time pi-up runs. Same
  ordering risk as today.
- **Discovery / multicast group.** Both `netmanager` and `netadapter` use
  the same default multicast group, but if `installer/jackbridge-pin-route`
  or pi-side routing pin is interface-specific, double-check it still
  carries the discovery traffic after the role flip.
- **HAL channel-count assumption is unchanged** (still 4-in / 2-out), so
  no shm-layout impact and no `JACKBRIDGE_PROTOCOL_VERSION` bump.
- **Reversion** is `git checkout` of the three files + restart of jackd on
  both sides. No persistent state to clean up.

## Test plan after flipping

1. `journalctl -u jack` on pi: confirm `netmanager` loaded, no `netadapter`
   slip-ring messages (they should now appear in Mac `log stream` instead).
2. `log stream --predicate 'subsystem == "com.jackbridge"'` on Mac:
   confirm daemon `auto-wire:` lines fire for `netadapter:capture_*` and
   `netadapter:playback_*`.
3. Empty pedalboard, 30 min: count Mac-side ringbuffer-failure / too-slow
   events. If the ramp pattern disappears, the flip helped. If it
   reappears on the Mac with the same monotonic shape, the resampler
   itself (not its host CPU) is the bottleneck.
4. With load (typical pedalboard): repeat. Pi xruns should now be rare;
   any remaining clicks come from Mac SRC or hops 8/9 in JITTER.md.
