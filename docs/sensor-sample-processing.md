# Sensor sample processing

```text
validated ReceivedPacket
          |
          v
SensorSampleProcessor
          |
          | decode_sensor_payload(message_type, payload bytes)
          |
          +-- invalid payload --> count and reject
          |
          +-- valid payload ----> SensorSampleHandler
```

The sensor sample processor is the seam between DriveFlow's packet transport
model and its typed sensor-data model. The epoll receiver validates the common
packet envelope. Worker threads then use `SensorSampleProcessor` to decode the
type-specific payload and deliver a `SensorSample` to downstream code.

## Packet versus sensor sample

A `protocol::Packet` is a transport value. It contains the common wire header
and encoded payload bytes. Its header carries protocol details such as magic,
version, encoded length, and CRC32.

A `runtime::SensorSample` is a processing value. It contains:

| Field | Meaning |
| --- | --- |
| `sequence_number` | Order identifier copied from the packet header |
| `source_timestamp_ns` | Monotonic time at which the source produced the sample |
| `receive_timestamp_ns` | Monotonic time at which Runtime received the datagram |
| `source` | Remote IPv4 address and UDP source port |
| `listener` | Local Runtime endpoint that received the datagram |
| `payload` | `ImuSample`, `GnssFix`, or `CameraMeta` in a `SensorPayload` variant |

Packet-envelope fields are not copied into `SensorSample` after they have
served their validation purpose. Downstream handlers therefore work with typed
measurements and relevant observation metadata instead of wire-format details.

## Two validation layers

DriveFlow deliberately validates in two stages.

The epoll receiver performs packet-level validation on the I/O thread:

- complete common header;
- correct magic and supported protocol version;
- known message type;
- payload length within the packet limit;
- encoded length matching the UDP datagram;
- matching CRC32.

The sensor sample processor performs payload-level validation on a worker:

- exact IMU and GNSS payload sizes;
- consistent CameraMeta fixed and extra-data lengths;
- finite numeric values;
- GNSS coordinates and accuracy values within their allowed ranges;
- non-zero camera dimensions.

Keeping type-specific decoding on workers prevents this processing from being
added to the latency-sensitive epoll receive path.

A packet can pass the first layer and fail the second. For example, an IMU
packet with a one-byte payload can have a completely valid packet length and
CRC. It is still not a valid IMU sample because an IMU payload requires the
six encoded floating-point values defined by the sensor schema.

## Public interface

Construct a processor with one non-empty handler:

```cpp
driveflow::runtime::SensorSampleProcessor processor(
    [](const driveflow::runtime::SensorSample& sample) {
      consume_sample(sample);
    });
```

Process a packet that has already passed packet-level validation:

```cpp
const auto error = processor.process(received_packet);
if (error != driveflow::protocol::SensorPayloadError::kNone) {
  report_invalid_sensor_payload(error);
}
```

`process()` returns `SensorPayloadError::kNone` after successful decoding and a
normal handler return. For an invalid payload, it returns the exact payload
error and does not invoke the handler.

The `SensorSample` reference passed to the handler remains valid only for the
duration of that call. A handler that needs the sample later must copy it into
storage that it owns.

## Concurrency

Runtime workers may call the same `SensorSampleProcessor` concurrently. Its
internal counters use relaxed atomic operations, so `process()` and `metrics()`
are safe to call from multiple threads.

This does not make arbitrary handler state thread-safe. A handler that mutates
shared state must synchronize that state itself:

```cpp
std::mutex samples_mutex;
std::vector<driveflow::runtime::SensorSample> samples;

driveflow::runtime::SensorSampleHandler handler =
    [&](const driveflow::runtime::SensorSample& sample) {
      const std::scoped_lock lock(samples_mutex);
      samples.push_back(sample);
    };
```

With more than one worker, handler completion order is not guaranteed. The
processor does not claim per-source ordering. A later source-tracking module
must choose an explicit ordering strategy before interpreting sequence-number
changes.

## Handler failures

The processor deliberately does not catch an exception thrown by the
`SensorSampleHandler`. When used by `Runtime`, the exception reaches the
`WorkerPipeline`, which:

- increments `handler_failures`;
- treats that packet's downstream handling as failed;
- keeps the worker alive for later packets;
- prevents the exception from escaping the worker thread.

The sample was still successfully decoded before the handler failed. It is
therefore included in `samples_decoded` and its type counter, while the worker
pipeline records the downstream failure separately.

## Metrics

`SensorSampleProcessorMetrics` contains:

| Metric | Meaning |
| --- | --- |
| `packets_examined` | `ReceivedPacket` values passed to `process()` |
| `samples_decoded` | Payloads successfully decoded into `SensorSample` |
| `payloads_rejected` | Valid packet envelopes with invalid typed payloads |
| `imu_samples` | Successfully decoded IMU samples |
| `gnss_samples` | Successfully decoded GNSS samples |
| `camera_meta_samples` | Successfully decoded camera metadata samples |

After all active calls finish, these invariants hold:

```text
packets_examined = samples_decoded + payloads_rejected

samples_decoded = imu_samples + gnss_samples + camera_meta_samples
```

During active concurrent processing, `metrics()` returns a monotonic snapshot.
Individual counters may represent slightly different instants until all calls
quiesce. `Runtime::run` snapshots the metrics only after its worker pipeline has
drained, so the returned summary is stable and satisfies both invariants.

## Runtime integration

`Runtime::run` accepts a `SensorSampleHandler`, not a raw packet handler:

```cpp
driveflow::runtime::Runtime runtime(config);
const auto summary = runtime.run(
    [] { return application_should_stop(); },
    [](const driveflow::runtime::SensorSample& sample) {
      process_typed_sample(sample);
    });
```

Internally, Runtime constructs one processor for the run and uses it as the
worker pipeline's packet-processing function:

```text
EpollReceiver
    |
    | ReceivedPacket
    v
SensorStreamTracker
    |
    | receive-order sequence observation
    v
WorkerPipeline
    |
    | worker thread
    v
SensorSampleProcessor
    |
    | SensorSample
    v
SensorSampleHandler
```

Runtime stops polling first, drains accepted worker work, then snapshots the
sample metrics. This lifetime order ensures that no worker can still access the
processor after it has been destroyed.

Sequence classification happens before this worker path and does not delay
typed decoding. Runtime exposes its aggregate `stream_metrics` separately.
See [sensor stream tracking](sensor-stream-tracking.md) for the source identity
and ordering contract.

The `--count` option continues to count valid packet envelopes considered by
Runtime. It includes packets later rejected for an invalid typed payload and
packets dropped because the worker queue was full. This makes bounded input
experiments terminate after the requested amount of network input.

## CLI summary

The final `driveflow_runtime` summary adds:

```text
packets_examined=10 samples_decoded=9 payloads_rejected=1 \
imu_samples=4 gnss_samples=3 camera_meta_samples=2
```

Read these together with the earlier layers:

- receiver `rejected` means the packet envelope or CRC was invalid;
- pipeline `dropped_queue_full` means valid input could not enter the worker
  queue;
- sample `payloads_rejected` means the envelope was valid but its sensor schema
  was invalid;
- pipeline `handler_failures` means decoding succeeded but downstream handling
  threw an exception.

## Scope boundaries

The sensor sample processor itself deliberately does not provide:

- Sensor Source identity beyond carrying the observed UDP endpoints;
- sequence-gap, duplicate, or reordering detection;
- per-source ordering partitions;
- rate or end-to-end latency aggregation;
- source-health state;
- shared-memory distribution;
- recording or replay.

Runtime's `SourceHealthMonitor` performs source identity and sequence
classification upstream, before WorkerPipeline dispatch. When this processor
returns a typed-payload error, the worker reports that issue back to the
monitor. This integration does not alter the processor's focused
packet-to-sample responsibility. See
[source health monitoring](source-health-monitoring.md).

The remaining downstream capabilities consume `SensorSample` values at this
seam. They do not need to understand packet byte order, CRC placement, or each
payload's binary layout.
