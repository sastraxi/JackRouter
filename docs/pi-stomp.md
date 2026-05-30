# pi-stomp setup (device side)

What needs to be true on the Raspberry Pi for JackBridge testing. The Mac side is `macos-setup.md`; this doc is everything on the other end of the Ethernet cable.

## Hardware in the test rig

- Raspberry Pi running pi-stomp (Arch Linux ARM, kernel `6.18.x-rpi-rt-v8-rt` PREEMPT_RT).
- Onboard ALSA audio at `hw:0` (the pi-stomp HAT — what local jackd drives in normal operation).
- `end0` (built-in Ethernet) wired directly to the Mac. `wlan0` is also up and provides the management path (`pistomp.local` mDNS).

## Networking

`pistomp.local` resolves to **`wlan0`** (DHCP from the LAN router), so ssh / mDNS / web UI all go over Wi-Fi by default. Audio must not — netJACK2 over Wi-Fi underruns even at modest buffer sizes (see `spike-b-clock-stability.md`).

For the test rig the Ethernet pair is link-local IPv4:

| Side | Interface | Address |
|---|---|---|
| Mac | `en7` (USB-C dongle or built-in) | `169.254.161.114/16` (auto, IPv4LL) |
| Pi  | `end0` | `169.254.161.200/16` (manual) |

The Mac's address was picked by IPv4LL (RFC 3927) as soon as the cable came up with no DHCP partner. The pi's value is arbitrary — anything in `169.254.1.0`–`169.254.254.255` that doesn't collide with the Mac works. Third octet matched (`.161`) purely for human readability.

### Bring up the pi's wired address

Currently ephemeral, by hand:

```bash
sudo ip addr add 169.254.161.200/16 dev end0
```

Verify from the Mac:

```bash
ping -c 3 -S 169.254.161.114 169.254.161.200   # ~0.5 ms, 0% loss expected
```

**TODO:** make this persistent. Arch uses `systemd-networkd` — a `/etc/systemd/network/10-end0-static.network` with `[Network] Address=169.254.161.200/16` survives reboot. Not done yet; we re-add by hand each session.

## Stopping the stock pi-stomp jackd

In normal pi-stomp operation, jackd runs as a systemd unit (`jack.service`, user `jack`) with the ALSA backend driving the pi-stomp HAT:

```
/usr/bin/jackd -t 2000 -R -P 75 -d alsa -d hw:0 -r 48000 -p 64 -n 2 -X seq -s
```

We need to stop it (and the pi-stomp host stack that sits on top) before running our own. The stock unit runs as user `jack` with `JACK_PROMISCUOUS_SERVER=jack`; cross-user client access from `pistomp` is fiddly enough that stopping and re-running as `pistomp` is the path of least resistance (Spike B finding).

```bash
sudo systemctl stop mod-ala-pi-stomp mod-ui mod-host mod-amidithru jack
```

## Starting the netJACK2 slave

Spike B's tested topology: pi runs `jackd -d alsa` driving `hw:0` as its own clock domain, then loads `netadapter` as an internal JACK client. `netadapter` (slave variant of the net backend) does the SRC across the Pi-ALSA / netJACK2 clock boundary — that's the SRC PLAN.md keeps reminding us *not* to put in JackBridge, because it lives here instead.

```bash
nohup jackd -R -P 75 -d alsa -d hw:0 -r 48000 -p 128 -n 2 > /tmp/jackd-pi.log 2>&1 &
sleep 2
jack_load netadapter -i "-C 2 -P 2 -a 169.254.161.114"
```

- `-P 75`: matches Spike B. Lower priorities underflow the net master deadline on the Mac side; the pi's tolerance is less brittle but we keep them symmetric.
- `-p 128`: matches `docs/macos-setup.md` default. Bump in lockstep with the Mac side if you want larger buffers — they must agree.
- `-C 2 -P 2`: 2-in / 2-out, matching the pi-stomp wet/DI use case scope.
- `-a 169.254.161.114`: pins to the Mac's wired IP. See "Pinning netJACK2 to Ethernet" below for why this is the right knob. The outer `-i` on `jack_load` is "init string"; everything inside the quotes is parsed by netadapter itself.

Sanity check from the pi:

```bash
jack_lsp                    # should show system:*, netadapter:*
jack_lsp -c netadapter      # should show netadapter:capture_{1,2} / playback_{1,2}
```

## Teardown — restoring the pi-stomp stack

```bash
killall jackd
sudo systemctl start jack mod-host mod-ui mod-ala-pi-stomp mod-amidithru
```

Confirm `jack.service` came back up and pi-stomp's normal UI is reachable before walking away — the stack tolerates being stopped but the start ordering is fussy (`jack` must be `active` before `mod-host` will succeed).

## Pinning netJACK2 to Ethernet

netJACK2 discovers via IPv4 multicast (default group `225.3.19.154`). With both `wlan0` and `end0` up, multicast follows whatever interface the OS routes the group through — by default that's the interface with the default route, i.e. Wi-Fi. We have to override.

The net backend's actual flags (from `jackd -d net --help` on JACK2 1.9.22 — verify against your local version):

- `-a, --multicast-ip` — *Multicast address, **or explicit IP of the master**.* Default `225.3.19.154`. **This is the pinning knob.** Set it to the Mac's wired IP (e.g. `169.254.161.114`) and the slave talks unicast to that address; the kernel routes via `end0` because the `169.254.0.0/16` link-scope route on `end0` wins for that destination.
- `-p, --udp-net-port` — UDP port (default 19000). Rarely needs touching.
- `-M, --mtu` — MTU to the master (default 1500).
- `-C / -P` — number of audio in / out ports.
- `-i / -o` — number of *MIDI* in / out ports. **Despite the name, `-i` is not an interface flag.** Don't try to pin with it.

There is no interface-bind option in netJACK2. Pinning is purely via destination IP + the host routing table.

Pi slave (when started by hand for testing — uses unicast pin):

```bash
jack_load netadapter -i "-C 2 -P 2 -a 169.254.161.114"
```

Mac master (netmanager internal client, loaded after `jackd`):

```bash
jack_load netmanager
```

netmanager is the listener — it binds the master UDP port on all interfaces. The slave's unicast destination is what determines which wire the traffic actually traverses; no symmetric flag is needed on the master side.

### Routing gotcha (uid-dependent route lookup)

On this pi's Arch setup, `ip route get 169.254.161.114` as `pistomp` (uid 1000) returns the wlan0 default-gateway path; as root it correctly returns `dev end0`. Cause not nailed down (no `ip rule` entries explain it; possible per-uid systemd-networkd policy). In practice the netadapter runs in a context where the end0 link-scope route does win, but if you see traffic on Wi-Fi when you expected Ethernet, this is the first thing to check.

## Verifying audio is on the wire

After both sides come up, with Wi-Fi still enabled on the Mac:

```bash
# On the Mac
sudo tcpdump -i en7 -n udp port 19000 -c 20
```

If you see UDP traffic between `169.254.161.114` and `169.254.161.200`, you've pinned correctly. If you see nothing on `en7` but the JACK graph is alive, the traffic escaped to Wi-Fi — re-check the `-a` value matches the Mac's `en7` address, not its Wi-Fi address.
