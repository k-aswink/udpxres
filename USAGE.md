# udpx — Usage Guide

This covers every flag, both modes (server/client), the three run modes, logging output formats, and a few real scenarios you might actually use this for.

## Table of contents

- [Building](#building)
- [Server mode](#server-mode)
- [Client mode](#client-mode)
- [Run modes](#run-modes)
- [Payload size](#payload-size)
- [Rate pacing](#rate-pacing)
- [Live stats](#live-stats)
- [Logging (CSV + detailed log)](#logging-csv--detailed-log)
- [JSON summary output](#json-summary-output)
- [IPv4 / IPv6](#ipv4--ipv6)
- [Reading the results](#reading-the-results)
- [Common scenarios](#common-scenarios)
- [Exit behavior / signals](#exit-behavior--signals)
- [Troubleshooting](#troubleshooting)

## Building

```bash
make
```

or directly:

```bash
gcc -O2 -Wall -Wextra -o udpx udpx.c -lm
```

No other dependencies. It's one source file.

## Server mode

```bash
./udpx -s [-p port] [-4|-6]
```

| Flag | Meaning |
|---|---|
| `-p port` | Port to listen on. Default `50505`. |
| `-4` / `-6` | Bind to IPv4 or IPv6 explicitly. Default is IPv4. |

The server just validates and echoes back whatever it receives — it doesn't generate any traffic itself. It'll print a running packet count every 1000 packets processed, and a summary of total processed/invalid packets when you stop it.

It also rate-limits itself (currently a fixed internal cap) so a single client — or something hammering it that isn't even your client — can't turn it into an unbounded echo amplifier. If you see `Warning: rate limit exceeded, dropping packet` in the server output, that's what's happening.

Stop it with Ctrl+C (or `SIGTERM`) — it shuts down cleanly and prints final counts.

## Client mode

```bash
./udpx -c <host> [options]
```

`<host>` can be an IPv4 address, an IPv6 address, or a hostname — it goes through `getaddrinfo()`, so DNS resolution works.

Full flag list:

| Flag | Meaning |
|---|---|
| `-p port` | Server port. Default `50505`. |
| `-n count` | Send exactly this many packets, then stop. Default `1000`. |
| `-t seconds` | Run for this many seconds instead of a fixed count. |
| `--continuous` | Run until you hit Ctrl+C. Prints an interim stats block every 10 seconds. |
| `--size bytes` | Payload size, 1–65000 bytes. Default `64`. |
| `--rate pps` | Cap the send rate to roughly this many packets/sec. |
| `--live` | Print a live, continuously-updating stats line instead of just the final summary. |
| `--log <file>` | Write `<file>.csv` and `<file>.log` during the run (see below). |
| `--json <file>` | Write a JSON summary to `<file>` when the run ends. |
| `-4` / `-6` | Force IPv4 or IPv6 resolution instead of letting the resolver pick. |
| `-h`, `--help` | Print usage and exit. |
| `-v`, `--version` | Print version and exit. |

You can only pick **one** run mode at a time (`-n`, `-t`, `--continuous` — mixing them is an error, not a "last one wins" situation).

## Run modes

**Count mode** (default) — send exactly N packets, then print the summary and exit.

```bash
./udpx -c 192.168.1.100 -n 5000
```

**Duration mode** — send as fast as the socket allows (or paced, if `--rate` is set) for a fixed wall-clock time.

```bash
./udpx -c 192.168.1.100 -t 60
```

**Continuous mode** — keep going until you kill it. Prints an interim stats block every 10 seconds so you can watch it live without waiting for a final report.

```bash
./udpx -c 192.168.1.100 --continuous
```

Ctrl+C at any point (in any mode) triggers a clean shutdown and still prints the final results based on whatever was collected up to that point.

## Payload size

```bash
./udpx -c 192.168.1.100 --size 1500
```

A few sizes that are actually useful, not arbitrary:

- `--size 1` — about as close to a bare heartbeat as you can get. Useful for isolating CPU/interrupt overhead from actual data-transfer effects.
- `--size 64` — the default, roughly representative of small control-plane traffic.
- `--size 1500` — right around standard Ethernet MTU. Good for spotting fragmentation-related loss that doesn't show up at smaller sizes.
- `--size 8192` — comfortably over MTU, forces fragmentation on most paths. Useful if you specifically want to see how a path handles fragmented UDP (some firewalls/NATs handle this badly).

## Rate pacing

By default the client sends as fast as it can (limited by the round-trip wait for each packet, since it's a synchronous ping-pong per packet, not a fire-and-forget flood). If you want a controlled, predictable send rate instead:

```bash
./udpx -c 192.168.1.100 -t 30 --rate 100
```

This paces sends to roughly 100 packets/sec using `nanosleep`. Useful when you want to simulate a specific application's traffic rate rather than just maxing out what the link/loop can handle.

## Live stats

```bash
./udpx -c 192.168.1.100 -n 10000 --live
```

Updates a single status line every 100 packets with current sent/received/loss/RTT/jitter, so you don't have to wait for a 10,000-packet run to finish to see how it's going. If stdout isn't a terminal (e.g. you've piped it somewhere), it switches to printing plain lines instead of overwriting in place — safe to redirect to a file.

## Logging (CSV + detailed log)

```bash
./udpx -c 192.168.1.100 --continuous --log myrun
```

This creates two files, both written to once per second during the run:

**`myrun.csv`** — one row per second, meant for import into a spreadsheet or a quick `awk`/`pandas` pass:

```
Timestamp,Server,Port,PayloadSize,ElapsedTime,PacketsSent,PacketsReceived,PacketsLost,InvalidPackets,OutOfOrder,LossPct,MinRTT,AvgRTT,MaxRTT,StdDev,AvgJitter,MinJitter,MaxJitter
```

**`myrun.log`** — the same data in a fixed-width table meant to be read directly, plus a header block with the run parameters and a status column (`EXCELLENT`/`GOOD`/`DEGRADED`/`POOR`) so you can `grep` for problem windows in a long-running log:

```
Timestamp           |  Time | Status     |     Sent |     Recv |     Lost |   Loss% | Success% | ...
```

Both files are cumulative snapshots — each row shows the stats for the *entire run so far*, not just that one-second window. If you want per-window deltas, diff consecutive rows.

## JSON summary output

```bash
./udpx -c 192.168.1.100 -n 5000 --json result.json
```

Writes a single JSON object when the run finishes — meant for CI checks, monitoring scripts, or anything that wants a final number instead of parsing the human-readable output:

```json
{
  "server": "192.168.1.100",
  "port": 50505,
  "payload_size": 64,
  "elapsed_seconds": 5,
  "packets_sent": 5000,
  "packets_received": 5000,
  "packets_lost": 0,
  "invalid_packets": 0,
  "out_of_order": 0,
  "loss_pct": 0.0,
  "rtt_ms": { "min": 0.1, "avg": 0.3, "max": 2.1, "stddev": 0.15, "p50": 0.28, "p95": 0.6, "p99": 1.1 },
  "jitter_ms": { "min": 0.0, "avg": 0.05, "max": 0.4, "samples": 4999 }
}
```

This plays nicely with `--log` — you can use both in the same run if you want the time-series CSV *and* a final-state JSON summary.

## IPv4 / IPv6

```bash
./udpx -c ::1 -6 -n 100      # force IPv6
./udpx -c 192.168.1.100 -4   # force IPv4
./udpx -c example.com        # let the resolver pick
```

Without `-4`/`-6`, it resolves whatever the system's default preference is. If you're testing a dual-stack host and want to be sure which family you're actually measuring, force it explicitly — don't assume.

## Reading the results

- **Loss rating** thresholds: <0.1% Excellent, <1% Good, <5% Fair, otherwise Poor.
- **Jitter rating**: <1ms Excellent, <5ms Good, <10ms Fair, otherwise Poor.
- **Latency rating**: <1ms Excellent, <10ms Good, <50ms Fair, otherwise Poor.

These are generic thresholds, not tuned for any specific application. If you're testing VoIP, gaming, or anything latency-sensitive, use the actual RTT/jitter/loss numbers and compare them against what *that* application actually needs — a "Fair" jitter rating here might be completely fine for a file sync and completely unacceptable for a voice call.

**Percentiles matter more than the average.** A run can have a great average RTT and still have a P99 that's 10x worse — that tail is usually what users actually notice. Always check P95/P99 before calling a link "good."

## Common scenarios

**"Is this VPN adding meaningful latency?"**

```bash
./udpx -c direct-host -n 2000 --json direct.json
./udpx -c vpn-host -n 2000 --json vpn.json
```
Compare the two JSON files — specifically P50 and P99, not just the average.

**"Something's dropping packets intermittently, but I don't know when"**

```bash
./udpx -c problem-host --continuous --log incident
```
Let it run in the background, come back later, and `grep -v EXCELLENT incident.log` to jump straight to the windows where it degraded.

**"Does this path handle a real MTU-sized packet, or is something silently dropping fragments?"**

```bash
./udpx -c host --size 1500 -t 30
./udpx -c host --size 8192 -t 30
```
If the 1500-byte run is clean but the 8192-byte run shows real loss, you're likely looking at a fragmentation problem somewhere on the path (common with tunnels/overlays that don't handle fragmented UDP well).

**"I want a single pass/fail number for a CI health check"**

```bash
./udpx -c host -n 500 --json ci_result.json
```
Then check `loss_pct` and `rtt_ms.p99` in the JSON against whatever thresholds matter for your pipeline.

## Exit behavior / signals

`SIGINT` (Ctrl+C), `SIGTERM`, and `SIGHUP` all trigger a clean shutdown — the socket is closed, and (in client mode) the summary is printed based on whatever was collected before the signal arrived. `SIGPIPE` is ignored so a half-closed pipe on the output side won't kill the process mid-run.

## Troubleshooting

**`Address family not supported by protocol` when using `-6`**
Your kernel/container has IPv6 disabled (check `/sys/module/ipv6/parameters/disable`). This isn't the tool failing — it's correctly reporting that the OS won't give it an IPv6 socket.

**Every packet shows up as `invalid`**
This means the CRC32 check is failing on receipt — almost always because the server and client binaries are different versions with a wire-format mismatch. Rebuild both from the same source.

**`Timeout on packet N` messages during a run**
The client waited past its receive timeout for that specific packet and counted it as lost. Occasional ones under real network loss are expected; a lot of them in a row on a local/LAN test usually means the server isn't reachable at all (wrong port, firewall, or the server process isn't running).

**High loss shows up only under `--rate` with a high pps value**
You may be exceeding the server's internal rate cap, not actually losing packets on the network. Check the server's own console output for "rate limit exceeded" warnings before assuming it's a network problem.
