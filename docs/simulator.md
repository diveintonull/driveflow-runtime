# Sensor simulator

`driveflow_sensor_simulator` produces a continuous stream of DriveFlow packets over IPv4 UDP. By
default every packet is valid and sent once in Sequence Number order. Optional deterministic fault
rules can drop, duplicate, reorder, delay, or corrupt selected generated packets. One process
represents one Sensor Source. Start separate processes for IMU, GNSS, and CameraMeta streams.

## Build and help

```bash
cmake --preset debug
cmake --build --preset debug
./build/debug/driveflow_sensor_simulator --help
```

## Examples

Send IMU packets at the default 200 Hz until interrupted:

```bash
./build/debug/driveflow_sensor_simulator --sensor imu
```

Send ten GNSS packets at 5 Hz to port 9100:

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor gnss \
  --port 9100 \
  --rate-hz 5 \
  --count 10
```

Send CameraMeta packets with 4 KiB of deterministic extra payload:

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor camera \
  --camera-extra-bytes 4096
```

Generate eight IMU packets with several repeatable transport faults:

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor imu \
  --port 9100 \
  --rate-hz 1000 \
  --count 8 \
  --drop-every 5 \
  --duplicate-every 3 \
  --reorder-every 2 \
  --delay-every 4 \
  --delay-ms 5
```

Press `Ctrl+C` to stop a continuous run. `SIGINT` and `SIGTERM` request a graceful stop; rate waits
and injected delay waits check that request every 10 ms.

## Options

| Option | Required | Default | Meaning |
| --- | --- | --- | --- |
| `--sensor <imu\|gnss\|camera>` | yes | none | Sensor Source type |
| `--destination <IPv4>` | no | `127.0.0.1` | UDP destination address |
| `--port <port>` | no | `9000` | UDP destination port |
| `--rate-hz <frequency>` | no | type-specific | Packet frequency |
| `--count <packets>` | no | continuous | Stop after generating this many logical packets |
| `--camera-extra-bytes <bytes>` | no | `0` | CameraMeta-only opaque load bytes |
| `--drop-every <N>` | no | disabled | Drop every Nth generated packet |
| `--duplicate-every <N>` | no | disabled | Send every Nth generated packet twice |
| `--reorder-every <N>` | no | disabled | Send every Nth packet after the next sendable packet |
| `--delay-every <N>` | no | disabled | Select every Nth packet for additional delay |
| `--delay-ms <milliseconds>` | no | disabled | Delay duration; requires `--delay-every` |
| `--corrupt-every <N>` | no | disabled | Corrupt every Nth packet after CRC encoding |
| `--help` | no | n/a | Print usage and exit |

The default rates are 200 Hz for IMU, 10 Hz for GNSS, and 30 Hz for CameraMeta. Frequencies must
be finite, greater than zero, and no more than 1,000,000 Hz. Camera extra data cannot exceed the
protocol payload limit. Every `N` interval must be a positive integer. `--delay-every` and
`--delay-ms` must be supplied together, and the delay must be between 1 and 60,000 milliseconds.

## Packet behavior

- Sequence Numbers start at `1` for each simulator process and increase once per generated packet,
  including a packet that is later dropped.
- Source Timestamps come from `std::chrono::steady_clock` and use nanoseconds.
- Each Sensor Sample is encoded by the existing typed payload codec, then wrapped by the existing
  Packet codec, including CRC32.
- CameraMeta `frame_id` matches the packet Sequence Number.
- Camera extra bytes use a deterministic repeating `0..255` pattern.
- `--rate-hz` controls logical packet generation, not the number of UDP datagrams after duplication.
- `--count` omits the final rate wait and exits after the requested number of logical packets has
  been generated and any finite-run reorder buffer has been flushed.

With fault injection disabled, generated packet count and sent datagram count remain equal, so the
original simulator behavior is unchanged.

## Deterministic fault model

All periodic rules are evaluated against the generated packet Sequence Number:

```text
triggered = sequence_number % N == 0
```

There is no random-number generator and no seed. The same configuration and packet count therefore
make the same sequence decisions on every run.

### Drop

`--drop-every N` prevents every Nth generated packet from reaching the UDP socket. The Sequence
Number is still consumed, so a receiver can observe a forward gap. Drop has priority over every
other fault: a dropped packet is not also duplicated, reordered, delayed, or corrupted.

### Duplicate

`--duplicate-every N` sends the selected encoded byte vector twice, back to back. Both datagrams have
the same Sequence Number, timestamp, payload, and CRC. A stream tracker should classify the second
arrival as a duplicate.

### Reorder

`--reorder-every N` implements a bounded adjacent-style swap:

1. the selected packet is held in one in-memory slot;
2. the next packet that is not dropped is sent first;
3. the held packet is sent immediately afterward.

For example, `--reorder-every 2 --count 5` emits Sequence Numbers:

```text
1, 3, 2, 5, 4
```

A dropped packet cannot act as the swap partner, so the held packet waits for the next sendable one.
Only one packet is ever retained. If a finite run ends with an unpaired held packet, the simulator
flushes it normally and does not count a completed reorder event.

### Delay

`--delay-every N --delay-ms M` adds an interruptible wait before the selected packet is emitted. If
that packet is also duplicated, the delay occurs once and the two copies are then sent back to back.
If it was held for reordering, its delay occurs when the held packet is finally emitted.

The normal source schedule remains anchored to its steady-clock deadlines. A long injected delay can
therefore make later generated packets catch up instead of permanently shifting every future
deadline. `Ctrl+C`, `SIGINT`, and `SIGTERM` can interrupt the delay within 10 milliseconds.

### Corruption

`--corrupt-every N` first creates a completely valid encoded packet and CRC, then flips one bit in
the final packet byte. This deliberately leaves the stored CRC stale. `decode_packet()` therefore
reports `kCrcMismatch`, and `EpollReceiver` rejects the datagram before stream tracking and worker
dispatch. If the same packet is also duplicated, both transmitted copies are corrupted.

## Fault composition order

When several rules select the same Sequence Number, the effective order is:

```text
generate and encode a valid packet
              |
              v
       drop? --yes--> discard
              |
              no
              v
          corrupt bytes
              |
              v
       hold/release reorder
              |
              v
          injected delay
              |
              v
       send, then duplicate
```

This order is part of the Simulator interface. In particular, drop dominance prevents confusing
summary counts such as a packet being both dropped and corrupted.

## Summary metrics

On shutdown the CLI prints one machine-readable summary line:

```text
stopped packets_generated=8 packets_sent=9 packets_dropped=1 duplicate_packets_sent=2 reorder_events=2 delayed_packets=2 corrupted_packets_sent=0
```

| Metric | Meaning |
| --- | --- |
| `packets_generated` | Logical packets created before injection |
| `packets_sent` | UDP datagrams actually sent, including extra duplicate copies |
| `packets_dropped` | Generated packets removed by the drop rule |
| `duplicate_packets_sent` | Extra datagrams sent by duplication |
| `reorder_events` | Completed next-sendable/held-packet swaps |
| `delayed_packets` | Logical packets whose injected delay started |
| `corrupted_packets_sent` | Actual sent datagrams whose encoded bytes were corrupted |

For a finite run that is not interrupted during an injected wait:

```text
packets_sent = packets_generated - packets_dropped + duplicate_packets_sent
```

Reordering and delay change order or time, not count. Corruption changes validity, not count.

## End-to-end stream-tracker demonstration

Start Runtime in terminal 1:

```bash
./build/debug/driveflow_runtime \
  --listen 127.0.0.1:9100 \
  --count 9 \
  --workers 4
```

Run the eight-packet fault example in terminal 2. It emits this receive order:

```text
1, 3, 3, 2, 6, 6, 4, 7, 8
```

The simulator summary is the example shown above. The stable Runtime metrics are:

```text
received=9 datagrams=9 accepted=9 rejected=0
first_observations=1 in_order_observations=2 gap_observations=2
duplicate_observations=2 reordered_observations=2 missing_samples_inferred=3
```

Runtime also prints one per-source line containing these stable fields:

```text
status=DEGRADED packets_received=9 gaps=2 duplicates=2 reordered=2
missing_inferred=3 payloads_rejected=0 dropped_queue_full=0
```

The source is degraded because the most recent sequence issue is still inside
the default one-second recovery period when this short run ends.

The exact `epoll_wakeups` and queue high-watermark can vary with scheduling and are not part of this
demonstration's expected values. Source UDP port, rate, inactivity, and latency
also vary by run.

To demonstrate corruption, run Runtime without `--count`, then send four packets with
`--corrupt-every 2` and stop Runtime with `Ctrl+C`:

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor imu \
  --port 9100 \
  --rate-hz 1000 \
  --count 4 \
  --corrupt-every 2
```

Runtime reports two accepted and two rejected datagrams. Only valid Sequence Numbers `1` and `3`
reach stream tracking, so they produce one first observation and one gap with one inferred missing
sample. The admitted source is degraded by that gap. The two CRC failures remain
in aggregate receiver rejection metrics because a packet envelope that cannot
be trusted cannot always provide the message type required for Sensor Source
identity.
