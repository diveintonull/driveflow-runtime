# Runtime receiver

```text
Sensor simulators
       |
       | UDP datagrams
       v
Non-blocking sockets
       |
       v
Single-threaded epoll receiver
       |
       | packet header, length, version, and CRC validated
       v
SensorStreamTracker
       |
       | source identified and sequence classified in receive order
       v
Bounded worker queue
       |
       v
Worker threads
       |
       v
SensorSampleProcessor
       |
       | typed SensorSample
       v
SensorSampleHandler
```

The DriveFlow runtime receiver is the long-running network entry point for
sensor packets. It uses Linux non-blocking UDP sockets and one level-triggered
`epoll` instance to monitor one or more listen endpoints.

The receiver deliberately stops at the packet boundary. It validates the
DriveFlow packet envelope and CRC, but it does not decode IMU, GNSS, or camera
payloads. `SensorSampleProcessor` performs payload decoding on worker threads,
outside the latency-sensitive I/O receive path.

Runtime classifies source sequence numbers on the same I/O thread before
parallel dispatch. See [sensor stream tracking](sensor-stream-tracking.md) for
source identity, wrap-around, bounded history, and metric semantics.

The Runtime moves valid packets into a bounded queue so slow processing cannot
block socket draining or grow memory without limit. See
[bounded worker pipeline](worker-pipeline.md) for its overload, concurrency,
shutdown, and metrics semantics.

## Build

```bash
cmake --preset debug
cmake --build --preset debug
```

The runtime executable is:

```text
build/debug/driveflow_runtime
```

The earlier `driveflow_udp_receiver` executable remains available as a
one-datagram protocol demonstration. `driveflow_runtime` is the
continuous receiver.

## Quick start

Start the runtime on the default endpoint, `0.0.0.0:9000`:

```bash
./build/debug/driveflow_runtime
```

In another terminal, start an IMU simulator:

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor imu \
  --destination 127.0.0.1 \
  --port 9000
```

Stop either process with `Ctrl+C`.

For a bounded demonstration, ask both processes to handle exactly ten packets:

```bash
./build/debug/driveflow_runtime \
  --listen 127.0.0.1:9000 \
  --count 10
```

```bash
./build/debug/driveflow_sensor_simulator \
  --sensor gnss \
  --destination 127.0.0.1 \
  --port 9000 \
  --count 10
```

## Multiple listen endpoints

Repeat `--listen` to register multiple UDP sockets with the same
`epoll` instance:

```bash
./build/debug/driveflow_runtime \
  --listen 127.0.0.1:9001 \
  --listen 127.0.0.1:9002 \
  --listen 127.0.0.1:9003
```

Each accepted packet records both:

- the remote UDP source endpoint;
- the local endpoint that received the datagram.

The pair is useful when several sensor sources share a message type or when
traffic is separated by port.

## Sensor Source identity

Protocol v1 has no explicit source ID. For one Runtime run, source tracking uses
this identity:

```text
(remote endpoint, local listener, message type)
```

This separates different senders, different configured Runtime inputs, and
independently sequenced sensor types sent from one socket. It is not a durable
hardware or restart identity.

The source table is bounded. `--max-sources` sets the number of admitted source
states. A packet from a new source after the table is full is counted as
untracked but continues into the worker pipeline; packets from already admitted
sources continue to be classified.

## Command-line options

| Option | Meaning | Default |
| --- | --- | --- |
| `--listen <IPv4:port>` | UDP endpoint to bind; repeatable | `0.0.0.0:9000` |
| `--count <packets>` | Stop after receiving this many valid packets | continuous |
| `--poll-timeout-ms <ms>` | Maximum `epoll_wait` duration | 100 |
| `--max-drain <datagrams>` | Maximum datagrams drained from one ready socket per poll | 64 |
| `--max-sources <sources>` | Maximum Sensor Sources retained by sequence tracking | 256 |
| `--workers <threads>` | Worker thread count | 2 |
| `--queue-capacity <packets>` | Maximum packets waiting for workers | 1024 |
| `--help` | Print usage | none |

Ports, counts, timeouts, drain limits, source limits, worker counts, and queue
capacities must be positive. Singleton options cannot be repeated. `--listen` is the only
repeatable option.

## Packet acceptance

For every UDP datagram, the receiver runs the packet decoder. A datagram is
accepted only when all packet-level checks pass:

- header is complete;
- magic value is correct;
- protocol version is supported;
- message type is known;
- payload length is within the protocol limit;
- encoded length matches the datagram length;
- CRC32 matches.

A rejected datagram increments the rejection metric and is discarded. It does
not terminate the process, and later datagrams on the same socket remain
receivable.

A packet can pass this layer even if its type-specific sensor payload is
invalid. Runtime's `SensorSampleProcessor` performs this second validation on
a worker, outside the I/O receive path. An invalid typed payload increments the
sample rejection metric and does not reach the `SensorSampleHandler`.

## Receiver metadata

Each `ReceivedPacket` contains:

| Field | Meaning |
| --- | --- |
| `packet` | Decoded DriveFlow packet |
| `source` | Remote IPv4 address and UDP source port |
| `listener` | Local endpoint that received the datagram |
| `receive_timestamp_ns` | Monotonic runtime receive timestamp |

The source timestamp in the packet describes when the simulator produced the
sample. The receive timestamp describes when the runtime obtained it. Keeping
both enables later latency calculations.

## Metrics

The final CLI summary reports:

| Metric | Meaning |
| --- | --- |
| `received` | Valid packets considered by Runtime for worker submission |
| `datagrams` | UDP datagrams read from all sockets |
| `accepted` | Datagrams that passed packet-level validation |
| `rejected` | Datagrams rejected by packet-level validation |
| `epoll_wakeups` | Poll calls that returned at least one ready descriptor |
| `streams_observed` | Sensor Sources admitted into the bounded tracker |
| `packets_observed` | Valid packet envelopes classified by the tracker |
| `first_observations` | First packet seen for admitted sources |
| `in_order_observations` | Exact next sequence numbers |
| `gap_observations` | Forward sequence jumps larger than one |
| `duplicate_observations` | Sequence numbers found in recent history |
| `reordered_observations` | Previously unseen older sequence numbers |
| `untracked_observations` | Packets from new sources after tracker capacity was full |
| `missing_samples_inferred` | Sequence numbers skipped when gaps were first observed |
| `submitted` | Packets accepted by the worker queue |
| `processed` | Packet processor calls that returned normally |
| `dropped_queue_full` | Incoming packets rejected because the queue was full |
| `rejected_stopped` | Submissions rejected after pipeline shutdown began |
| `handler_failures` | Packet processor calls that threw |
| `queue_high_watermark` | Largest observed number of waiting packets |
| `packets_examined` | Worker packets examined by the sensor sample processor |
| `samples_decoded` | Payloads decoded into typed sensor samples |
| `payloads_rejected` | Valid packet envelopes with invalid typed payloads |
| `imu_samples` | Successfully decoded IMU samples |
| `gnss_samples` | Successfully decoded GNSS samples |
| `camera_meta_samples` | Successfully decoded camera metadata samples |

A timeout is not counted as an `epoll` wakeup.

After a graceful drain, `submitted` equals `processed + handler_failures`.
`received` equals `submitted + dropped_queue_full` in the normal Runtime
path.

Stable stream metrics satisfy:

```text
packets_observed =
    first_observations
  + in_order_observations
  + gap_observations
  + duplicate_observations
  + reordered_observations
  + untracked_observations
```

Stable sample metrics also satisfy:

```text
packets_examined = samples_decoded + payloads_rejected
samples_decoded = imu_samples + gnss_samples + camera_meta_samples
```

## Why level-triggered epoll

The receiver uses level-triggered `epoll`. A ready socket remains
observable while unread datagrams are queued. This is easier to reason about
than edge-triggered operation and reduces the chance of leaving data stranded
because one drain loop stopped too early.

After an `EPOLLIN` event, the receiver repeatedly calls the
non-blocking receive operation until either:

- the socket reports `EAGAIN`;
- the configured per-listener drain limit is reached.

The drain limit prevents a high-rate source from monopolizing the single I/O
thread. If data remains, level-triggered `epoll` reports that socket
again on a later poll.

## Graceful shutdown

The executable installs handlers for `SIGINT` and `SIGTERM`.
The handler only changes a `sig_atomic_t` stop flag. A signal interrupts
the current `epoll_wait` call, after which the Runtime loop observes
the flag and stops polling sockets.

Runtime then stops accepting pipeline submissions, drains every packet already
accepted by the queue, joins the workers, and prints final metrics. A processor
that never returns will therefore prevent graceful shutdown from completing.

## Library interface

Applications can use the Runtime without the command-line executable:

```cpp
driveflow::runtime::RuntimeConfig config;
config.receiver.listen_endpoints = {
    {.address = "127.0.0.1", .port = 9000},
};
config.pipeline = {
    .worker_count = 4,
    .queue_capacity = 4096,
};
config.max_sensor_sources = 256;
config.poll_timeout = std::chrono::milliseconds{100};

driveflow::runtime::Runtime runtime(config);
const auto summary = runtime.run(
    [] { return application_should_stop(); },
    [](const driveflow::runtime::SensorSample& sample) {
      process_sample(sample);
    });
```

Runtime decodes typed payloads on worker threads before invoking the handler.
The handler may run concurrently and in a different completion order from
packet arrival, so it must synchronize any shared mutable state.
`Runtime::run` returns only after accepted work has drained. See
[sensor sample processing](sensor-sample-processing.md) for validation,
concurrency, failure, and metric semantics.

## Scope boundaries

The Runtime processing path does not provide:

- per-source rate or latency aggregation;
- source-health state;
- durable source or source-session identity;
- automatic source expiration or tracker state eviction;
- per-source ordering partitions;
- batching with `recvmmsg`;
- persistent recording, replay, or shared-memory distribution.

Those capabilities can be added downstream without changing how sockets are
registered and drained.
