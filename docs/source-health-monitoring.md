# Source health monitoring

`SourceHealthMonitor` turns accepted packet observations and downstream
processing problems into bounded, per-source health snapshots.

```text
EpollReceiver
    |
    | accepted ReceivedPacket, receive order
    v
SourceHealthMonitor::observe_packet
    |       |
    |       +-- SensorStreamTracker classifies sequence numbers
    |       +-- recent receive rate and latency are updated
    |
    v
WorkerPipeline
    |       |
    |       +-- queue rejection -> kQueueDropped
    |       +-- typed payload rejection -> kPayloadRejected
    v
SourceHealthMonitor::report(now)
    |
    +-- UNKNOWN / HEALTHY / DEGRADED / OFFLINE per Sensor Source
```

## Source identity and bounded state

The monitor uses the same observable `SensorSource` identity as sequence
tracking:

```text
(remote IPv4 endpoint, local listener endpoint, message type)
```

Protocol v1 does not carry a durable hardware or boot-session identity. A
sender restart that reuses the same endpoint is therefore still the same
observable source for one Runtime run.

`max_sources` bounds both sequence state and health state. When capacity is
full, a packet from a new source is `kUntracked`: it remains in aggregate
stream metrics and continues toward the worker pipeline, but it does not
create a per-source health snapshot.

## State rules

Status is evaluated when `report(now_timestamp_ns)` is called. It is not stored
as a separately ticking background state machine.

Rules are applied in this order:

1. `OFFLINE` when time since the last accepted packet is at least
   `offline_after`.
2. `DEGRADED` when silence is at least `degraded_after`.
3. `DEGRADED` while recovering from a recent issue.
4. `UNKNOWN` until `healthy_after_packets` accepted packets have been seen.
5. `HEALTHY` otherwise.

The default configuration is:

| Setting | Default | Meaning |
| --- | --- | --- |
| `healthy_after_packets` | 2 packets | Evidence required before initial health is known |
| `degraded_after` | 500 ms | Silence that makes an active source degraded |
| `offline_after` | 2 s | Silence that makes a source offline |
| `recovery_after` | 1 s | Minimum issue-free time before recovery |

`offline_after` must be strictly greater than `degraded_after`. All durations
and counts must be positive.

Recovery requires both:

- at least `recovery_after` since the most recent issue;
- at least `healthy_after_packets` normal packet observations after that
  issue.

Requiring time and new evidence prevents a source from becoming healthy merely
because an old error aged out while no new packets arrived. Recovery transitions
are supported: an offline source can become healthy again after packets resume.

## What counts as an issue

The following observations degrade a source:

- a sequence gap;
- a duplicate sequence observation;
- a reordered sequence observation;
- a source timestamp later than Runtime's receive timestamp;
- a valid packet envelope whose typed sensor payload is rejected;
- a packet dropped because the worker queue is full.

The first and in-order sequence observations are normal. A downstream user
handler exception is not attributed to source health because the packet and
typed payload were valid; it remains a `WorkerPipeline` handler failure.

## Rate calculation

`current_rate_hz` uses the most recent 64 accepted packet timestamps for that
source:

```text
rate = (timestamp_count - 1) / (newest_timestamp - oldest_timestamp)
```

Only 64 timestamps are retained, so memory is constant per source. The value
is an observed packet-arrival rate, not an expected-rate compliance check.
Duplicates and reordered packets count because they consume real Runtime input
capacity. One timestamp, or several identical timestamps, reports `0.0` Hz
instead of dividing by zero.

The fixed observation count intentionally gives high-rate sources a shorter
time horizon than low-rate sources. A future metrics module may add
time-bucketed rates if a uniform time horizon becomes necessary.

## Latency calculation

For local simulators, both `source_timestamp_ns` and `receive_timestamp_ns` use
the Linux monotonic clock epoch. The monitor reports:

- `latest_latency_ns`;
- `maximum_latency_ns`;
- `timestamp_anomalies` when source time is later than receive time.

An invalid comparison reports zero latest latency, increments the anomaly
counter, and degrades the source. Cross-machine deployment would require clock
synchronization or a different latency model; protocol v1 does not solve that
problem.

Percentiles are deliberately not estimated here. They belong in the later
metrics module, where histogram resolution and memory policy can be chosen
explicitly.

## Threading and ordering

`observe_packet()` must be called in receiver order. It owns
`SensorStreamTracker`, so worker scheduling cannot be mistaken for network
reordering.

`observe_issue()` is thread-safe and may be called concurrently by workers.
`report()` is also thread-safe and returns a copied, stable snapshot sorted by
observable source identity. A mutex protects the bounded state because health
updates are low-cost control-plane work and a hand-written lock-free design
would add complexity without evidence of benefit.

Runtime captures the report time as soon as its receive loop stops, then drains
the worker pipeline and finally builds the report. This includes payload errors
discovered during drain without falsely treating a long drain as source
silence.

## Runtime configuration

The executable exposes liveness and recovery durations:

```text
--health-degraded-ms <ms>
--health-offline-ms <ms>
--health-recovery-ms <ms>
```

`--max-sources` continues to set the shared bound. Initial evidence remains two
packets in the Runtime interface; library users can construct a standalone
monitor with a different `healthy_after_packets` value.

## Snapshot fields

Each `SourceHealthSnapshot` contains:

| Field | Meaning |
| --- | --- |
| `source` | Observable source identity |
| `status` | Current evaluated health state |
| `packets_received` | Accepted packet envelopes observed for this source |
| `first_receive_timestamp_ns` | First Runtime receive timestamp |
| `last_receive_timestamp_ns` | Most recent Runtime receive timestamp |
| `inactivity_ns` | Report time minus last receive time, clamped at zero |
| `current_rate_hz` | Rate over up to 64 most recent arrivals |
| `latest_latency_ns` | Latest valid receive minus source timestamp |
| `maximum_latency_ns` | Largest valid observed latency |
| `timestamp_anomalies` | Source timestamps later than receive time |
| `gap_observations` | Forward sequence jumps larger than one |
| `duplicate_observations` | Recent sequence numbers seen before |
| `reordered_observations` | Previously unseen older sequence numbers |
| `missing_samples_inferred` | Sequence positions skipped by gaps |
| `payloads_rejected` | Type-specific payload validation failures |
| `packets_dropped_queue_full` | Source packets rejected by local backpressure |

The command-line program prints one `source_health` line per snapshot after the
aggregate Runtime summary.

## Packet-level rejection boundary

A datagram rejected for invalid magic, version, encoded length, message type,
or CRC never becomes a `ReceivedPacket`. Some failures also make the message
type untrustworthy, while message type is part of Sensor Source identity.

Those failures therefore remain in aggregate `ReceiverMetrics::packets_rejected`
instead of being falsely attributed to a per-source snapshot. Typed payload
failures happen after a valid packet and can be attributed precisely.

Per-endpoint rejection events or an explicit protocol source ID can be added
later if this distinction becomes important.

## Scope boundaries

This module does not provide:

- a live external CLI or IPC query channel;
- expected frequency profiles per sensor type;
- latency percentiles or histograms;
- durable source/session identity;
- source eviction and reuse of bounded slots;
- persistence across Runtime restarts;
- per-source attribution for undecodable packet envelopes.

It provides the stable in-process health model that a later CLI, metrics
collector, or health-event publisher can query.
