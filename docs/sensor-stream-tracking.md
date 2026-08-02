# Sensor stream tracking

```text
validated ReceivedPacket
          |
          | single Runtime I/O thread, receive order
          v
SourceHealthMonitor
          |
          +-- owns SensorStreamTracker
          +-- identifies Sensor Source
          +-- classifies sequence number
          +-- updates bounded stream and health state
          |
          v
WorkerPipeline
```

`SensorStreamTracker` turns packet-header sequence numbers into explicit stream
observations without confusing worker scheduling with network ordering. Runtime
calls it through `SourceHealthMonitor` after packet-envelope validation and
before parallel worker dispatch. The tracker remains independently usable.

## Sensor Source identity

DriveFlow protocol v1 has no explicit source ID. During one Runtime run, a
`SensorSource` is therefore identified by this tuple:

```text
(remote IPv4 endpoint, local listener endpoint, message type)
```

All three fields are required:

- the remote endpoint separates independent UDP senders;
- the local listener separates streams intentionally routed to different
  Runtime inputs;
- the message type allows one remote socket to carry independently sequenced
  IMU, GNSS, and CameraMeta streams.

This is an observed stream identity, not a durable hardware identity. If a
sender restarts while reusing the same endpoint and resets its sequence number,
protocol v1 cannot distinguish that restart from old reordered traffic. A
future protocol can add an explicit source and boot/session identity without
changing the sequence-classification algorithm.

## Sequence classifications

`observe()` returns one `SequenceObservation`:

| Status | Meaning |
| --- | --- |
| `kFirst` | First packet observed for this Sensor Source; no earlier loss is inferred |
| `kInOrder` | Exactly one step after the highest observed sequence number |
| `kGap` | A forward jump larger than one; `missing_samples` is the skipped count |
| `kDuplicate` | A sequence number already present in the recent history |
| `kReordered` | A previously unseen older sequence number arrived late |
| `kUntracked` | Source capacity was full, so no state was created for this source |

A gap is not proof of packet loss. For example, observing `10, 14, 13` first
produces a gap with three inferred missing numbers, then a reordered arrival.
The cumulative gap metric is historical and is not revised when late packets
arrive.

The first sequence number may be any value. Observing `900` as the first packet
does not imply that Runtime lost `1` through `899`, because Runtime may have
started after the source.

## Unsigned wrap-around comparison

Sequence numbers are 64-bit unsigned integers and eventually wrap from
`UINT64_MAX` to `0`. The tracker compares them in the modular sequence space:

```text
forward_distance = current - highest       (unsigned arithmetic)
```

- distance `0` is a duplicate;
- distances from `1` through `2^63 - 1` move forward;
- distance `1` is in order;
- larger forward distances are gaps;
- distances of `2^63` or more are older or exactly ambiguous and do not move
  the highest sequence number.

This means `UINT64_MAX - 1, UINT64_MAX, 0` is correctly classified as first,
in order, in order. The exact half-range case is intentionally treated as
reordered because neither direction is uniquely closer.

## Bounded duplicate history

Each admitted source stores:

- its highest sequence number;
- a 64-bit bitmap describing the most recent 64 sequence positions.

Bit zero represents the highest number. Older positions use higher bits. A
forward observation shifts the bitmap; a late observation checks and sets its
bit. This distinguishes a first late arrival from a repeated late arrival with
constant memory per source.

Traffic more than 63 positions behind the highest number is outside the recent
history. It is classified as reordered every time because claiming that it is
a duplicate would require unbounded history.

## Bounded source state

Runtime accepts untrusted UDP source endpoints, so the source table must not
grow forever. `SensorStreamTrackerConfig::max_sources` bounds admitted stream
state. Runtime exposes it as:

```text
--max-sources <sources>     tracked sensor source limit (default: 256)
```

When the table is full, a packet from a new Sensor Source is classified as
`kUntracked` and increments `untracked_observations`. Runtime still submits the
packet to its bounded worker pipeline. Capacity protects tracking state; it is
not an input filtering policy. Packets from already admitted sources continue
to be classified normally.

Zero capacity is rejected as invalid configuration.

## Why tracking happens before workers

Runtime workers may complete in a different order from packet arrival. If the
tracker ran inside `SensorSampleHandler`, a valid input sequence such as
`10, 11, 12` could be observed as `11, 10, 12` solely because worker 2 ran
before worker 1.

Runtime therefore uses this order:

```text
EpollReceiver
    |
    | validated packet, receive order
    v
SensorStreamTracker
    |
    v
WorkerPipeline
    |
    | parallel processing
    v
SensorSampleProcessor
```

The tracker intentionally has no internal mutex. Serializing concurrent worker
calls with a mutex would prevent a data race but would not restore the original
receive order. Its interface requires callers to supply one serial observation
order, and Runtime does so on its single I/O thread.

## Relationship to later validation and overload

The tracker observes every packet envelope considered by Runtime, before queue
submission and typed payload decoding. Therefore:

- a packet later dropped because the worker queue is full is still represented
  in stream metrics;
- a packet whose type-specific payload is later rejected is still represented
  in stream metrics;
- a malformed datagram rejected by `EpollReceiver` never reaches the tracker;
- a downstream handler exception does not change the earlier sequence result.

This separation distinguishes upstream stream behavior from local overload and
payload-schema failures.

## Public interface

```cpp
driveflow::runtime::SensorStreamTracker tracker(
    {.max_sources = 256U});

const auto observation = tracker.observe(received_packet);
if (observation.status == driveflow::runtime::SequenceStatus::kGap) {
  report_gap(observation.sequence_number, observation.missing_samples);
}

const auto metrics = tracker.metrics();
```

`identify_sensor_source(packet)` is also public when a caller needs the exact
identity tuple independently of sequence tracking.

`observe()` and `metrics()` are not thread-safe. They must be used from the
same serial observation context unless the caller provides external
synchronization that also preserves the intended order.

## Metrics

`SensorStreamMetrics` contains:

| Metric | Meaning |
| --- | --- |
| `streams_observed` | Sensor Sources admitted into the bounded state table |
| `packets_observed` | Valid packet envelopes passed to `observe()` |
| `first_observations` | First packet for an admitted source |
| `in_order_observations` | Exact next sequence numbers |
| `gap_observations` | Forward jumps larger than one |
| `duplicate_observations` | Sequence numbers found in recent history |
| `reordered_observations` | Previously unseen older sequence numbers |
| `untracked_observations` | Packets from new sources after capacity was full |
| `missing_samples_inferred` | Cumulative numbers skipped when gaps were first observed |

After observation calls finish, this invariant holds:

```text
packets_observed =
    first_observations
  + in_order_observations
  + gap_observations
  + duplicate_observations
  + reordered_observations
  + untracked_observations
```

`missing_samples_inferred` uses saturating addition and remains at
`UINT64_MAX` instead of wrapping if malicious or extreme jumps overflow the
counter.

`RuntimeSummary::stream_metrics` is stable when `Runtime::run` returns. Runtime
currently exposes aggregate stream metrics; the per-packet observation returned
by `observe()` is not added to `SensorSampleHandler` yet.

## Reproducible simulator input

The Sensor Simulator can now generate deterministic sequence behavior through
`--drop-every`, `--duplicate-every`, and `--reorder-every`. For example, its
documented eight-packet demonstration emits:

```text
1, 3, 3, 2, 6, 6, 4, 7, 8
```

The corresponding tracker metrics are:

```text
first_observations=1
in_order_observations=2
gap_observations=2
duplicate_observations=2
reordered_observations=2
missing_samples_inferred=3
```

`--corrupt-every` is intentionally different: it changes packet bytes after
CRC encoding. Those datagrams are rejected by `EpollReceiver` and therefore do
not appear in `packets_observed`. This makes it possible to demonstrate the
separation between malformed transport input and valid packets with unusual
sequence behavior.

See [Sensor simulator](simulator.md#end-to-end-stream-tracker-demonstration) for
the complete two-terminal commands and expected output.

Runtime now composes this tracker inside
[Source health monitoring](source-health-monitoring.md), which adds bounded
per-source rate, latency, issue counters, liveness, and health snapshots without
moving sequence comparison onto worker threads.

## Scope boundaries

The tracker deliberately does not provide:

- durable hardware or session identity;
- automatic source expiration or state eviction;
- confirmation that a gap became permanent loss;
- per-source ordered worker execution;
- thread-safe concurrent sequence observation;
- persistence across Runtime restarts.

Those remaining policies can build on the source identity and sequence
semantics defined here.
