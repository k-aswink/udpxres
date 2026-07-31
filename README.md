# udpx

A small UDP round-trip probe for Linux. You run a server on one box, point the client at it from another, and it tells you the RTT, jitter, loss, and reordering on that path — with percentiles, not just an average.

I built this because I kept reaching for `ping` when debugging UDP-specific issues (game servers, VoIP, custom protocols over UDP) and it wasn't telling me what I actually needed — ICMP behaves differently from UDP on a lot of paths, and `ping` doesn't let you pick a payload size or get P99 latency. `iperf3` is great for "how much bandwidth can this link push," but it's not really built to answer "what does the RTT distribution look like for my actual packet size." This sits in the gap between the two: it's a ping-pong style probe (client sends, server echoes, client times the round trip), single static binary, no dependencies beyond libm.

It's a single `.c` file. No external libraries beyond the standard math library. Should build on any modern Linux box with gcc.

## What it actually measures

- **RTT** — measured with `CLOCK_MONOTONIC`, so NTP adjustments mid-run won't corrupt your numbers.
- **Jitter** — RFC 3550-style, the variation between consecutive RTTs.
- **Percentiles** — P50 / P95 / P99, calculated from the full RTT history of the run, not estimated.
- **Packet loss** — with a real timeout, not just "assume it's lost after N seconds."
- **Out-of-order / reordering** — detected by sequence number.
- **Inter-packet delay** — how evenly spaced the responses actually arrived.
- **Payload integrity** — every packet is CRC32-checked (header *and* payload), so silent corruption on the wire gets flagged as an invalid packet instead of being counted as a clean round trip.

## Features

- **Three run modes**: fixed packet count, fixed duration, or continuous until you hit Ctrl+C.
- **Configurable payload size** from 1 byte up to 65000 bytes, so you can test small control-traffic packets or near-MTU-sized ones.
- **Client-side rate pacing** (`--rate`) if you want to send at a controlled pps instead of as fast as the socket allows.
- **Live stats mode** — a rolling one-line view of the run as it happens, instead of waiting for the final summary.
- **IPv4 and IPv6**, with hostname resolution (not just raw IP literals) via `getaddrinfo()`.
- **CSV + plain-text logging** — one flag (`--log`) writes out both a machine-parseable CSV time series and a human-readable detailed log, updated every second during the run.
- **JSON summary export** (`--json`) for feeding results into whatever automation or CI check you've already got.
- **Clean, scriptable output** — it detects whether stdout is a real terminal or has been redirected, and drops the fancy formatting automatically when piped to a file or another program.
- **Rate-limited server** — the server side caps how many packets/sec it'll echo, so it doesn't turn into an accidental amplifier if pointed at by something misbehaving.

## Quick start

Build it:

```bash
git clone https://github.com/yourname/udpx.git
cd udpx
make
```

(or just `gcc -O2 -Wall -Wextra -o udpx udpx.c -lm` if you don't want to bother with the Makefile)

Run the server on the box you want to test against:

```bash
./udpx -s -p 50505
```

Run the client from wherever you're testing from:

```bash
./udpx -c 192.168.1.100 -p 50505
```

That's the default: 1000 packets, 64-byte payload, and a full stats breakdown at the end.

## Example output

```
$ ./udpx -c 127.0.0.1 -p 51700 -n 200

==============================================================================
                        NETWORK PROBE - FINAL RESULTS
==============================================================================
Overall Status: EXCELLENT - Network performing optimally
------------------------------------------------------------------------------
PACKET STATISTICS
  Packets Sent             : 200
  Packets Received         : 200
  Packets Lost             : 0 (0.00%)
  Invalid Packets          : 0
  Out-of-Order Packets     : 0
------------------------------------------------------------------------------
ROUND-TRIP TIME (RTT) ANALYSIS
  Minimum                  : 0.006 ms
  Average                  : 0.010 ms
  Maximum                  : 0.519 ms
  Std Deviation            : 0.036 ms
  Percentiles              : P50=0.006 ms  P95=0.009 ms  P99=0.033 ms
------------------------------------------------------------------------------
JITTER (LATENCY VARIATION) ANALYSIS
  Minimum                  : 0.000 ms
  Average                  : 0.004 ms
  Maximum                  : 0.510 ms
  Samples                  : 199
------------------------------------------------------------------------------
NETWORK QUALITY ASSESSMENT
  Packet Loss Rating       : Excellent
  Jitter Rating            : Excellent
  Latency Rating           : Excellent
==============================================================================
```

## Full usage guide

See [USAGE.md](USAGE.md) for every flag, all three run modes, logging/JSON output formats, and a handful of real troubleshooting scenarios (packet loss over a VPN, MTU testing, comparing two paths, feeding results into scripts).

## How this is different from iperf/ping/etc.

Short version, since it comes up:

- **vs `ping`**: ICMP, not UDP — a lot of the things you're actually debugging (game traffic, VoIP, custom UDP protocols) behave differently on the wire than ICMP does, especially through NAT/firewalls. `ping` also won't give you percentiles or let you pick payload size.
- **vs `iperf3`**: iperf3 answers "how much throughput can this link sustain" by saturating it. This answers "what's the actual round-trip latency distribution for a packet of this specific size" — it's a latency/jitter probe, not a bandwidth benchmark. It doesn't try to max out your link.
- **vs `sockperf`**: closest relative in spirit (ping-pong RTT benchmarking) — this is a lighter, single-file version of that idea, with more built-in output formats.

The tradeoff: it speaks its own simple wire protocol, so you need `udpx` on both ends. It's not going to interoperate with an existing iperf or ping deployment.

## Requirements

- Linux (uses `CLOCK_MONOTONIC`, `getaddrinfo`, standard POSIX sockets — no non-Linux code paths)
- gcc (or any C11-compatible compiler)
- libm (standard on basically everything)

## License

Add whatever license you're using this project under — nothing is specified yet.

## Contributing

Issues and PRs welcome. If you're adding a feature, a quick note in the PR about what problem it solves is more useful to me than a big diff with no context.
