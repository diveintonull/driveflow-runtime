# Bounded worker pipeline

```text
epoll I/O thread
      |
      | try_submit(ReceivedPacket)
      v
bounded FIFO queue
      |
      +------------+------------+
      v            v            v
   worker 1     worker 2      worker N
      |            |            |
      +------------+------------+
                   |
                   v
             PacketProcessor
```

The worker pipeline separates network I/O from packet processing. The epoll
thread remains responsible for receiving and validating datagrams. Worker
threads run downstream packet processing without making the I/O thread wait
for that processing to finish.

The queue is bounded. A slow processor therefore cannot make memory usage grow
without limit.

## Runtime defaults

| Setting | Default | Meaning |
| --- | ---: | --- |
| worker threads | 2 | Maximum number of `PacketProcessor` calls that may run concurrently |
| queue capacity | 1024 | Maximum number of packets waiting for a worker |

A packet already executing on a worker does not occupy a queue slot. With two
workers and a queue capacity of 1024, the pipeline can therefore own up to 1026
packets at one time: two executing and 1024 waiting.

## Command-line configuration

Use `--workers` and `--queue-capacity` to override the defaults:

```bash
./build/debug/driveflow_runtime \
  --listen 127.0.0.1:9000 \
  --workers 4 \
  --queue-capacity 4096
```

Both values must be positive integers. `--workers` and `--queue-capacity` are
singleton options and cannot be repeated.

## Submission results

`WorkerPipeline::try_submit` returns one of three results:

| Result | Meaning |
| --- | --- |
| `kAccepted` | The pipeline owns the packet and guarantees that it will invoke the processor before `stop()` finishes |
| `kQueueFull` | The bounded queue had no free slot, so the incoming packet was discarded immediately |
| `kStopped` | Shutdown had started, so the packet was not accepted |

Submission does not wait for a worker or for queue space. It only holds the
pipeline's state lock long enough to inspect the state and, when possible,
move the packet into the queue.

The packet is passed by value so callers can move it into the pipeline:

```cpp
const auto result = pipeline.try_submit(std::move(packet));
```

After that call, the caller should not depend on the moved-from packet value,
regardless of the result.

## Queue-full policy

The pipeline rejects the newest packet when the queue is full. It does not:

- block the epoll thread;
- allocate an unbounded overflow queue;
- silently evict a packet that was accepted earlier;
- retry the rejected packet.

This policy gives `kAccepted` a strong meaning: once a packet is accepted, it
will not later be displaced by newer traffic.

Blocking the producer would not create useful end-to-end backpressure for UDP.
It would stop socket draining, move the backlog into the kernel receive buffer,
and eventually cause the kernel to drop datagrams without the Runtime knowing
which application-level work was lost.

Rejecting at the pipeline instead makes overload visible through a metric and
keeps the I/O path responsive.

## Processing concurrency

The supplied `PacketProcessor` may be called concurrently by several workers.
The processor must synchronize access to any shared mutable state.

For example, this processor is thread-safe because it protects the vector:

```cpp
std::mutex sequences_mutex;
std::vector<std::uint64_t> sequences;

driveflow::runtime::PacketProcessor processor =
    [&](const driveflow::runtime::ReceivedPacket& packet) {
      const std::scoped_lock lock(sequences_mutex);
      sequences.push_back(packet.packet.header.sequence_number);
    };
```

With more than one worker, processor completion order is not guaranteed. A
packet with sequence number 12 may finish before sequence number 11 even though
11 entered the queue first.

Use one worker when strict global FIFO processing is required. Per-source
parallelism with per-source ordering requires a later partitioned pipeline; it
is not implied by this worker pool.

## Processor failures

A C++ exception escaping from a thread entry function would normally terminate
the process. The pipeline therefore catches every exception thrown by the
processor.

When a processor throws:

- `handler_failures` increases;
- the failed packet is considered finished and is not retried;
- that worker continues with later queued packets;
- the exception does not cross into the epoll thread.

The pipeline records the count, not the exception text or type. Applications
that need structured error reporting should catch domain-specific exceptions
inside their processor and publish the details to their own observation path.

## Graceful stop

`WorkerPipeline::stop()` performs an orderly drain:

1. acquire the pipeline state lock;
2. stop accepting new packets;
3. wake every sleeping worker;
4. let workers finish all accepted packets;
5. join every worker thread;
6. return a stable metrics snapshot.

The operation is thread-safe and idempotent. Calling it again returns the same
completed state, except that later rejected submissions may have increased the
`packets_rejected_stopped` metric.

The destructor also calls `stop()`, so accepted work is drained during normal
RAII destruction even when the caller does not call it explicitly.

`stop()` must be called from outside the `PacketProcessor`. A worker cannot join
itself. Also, `stop()` can only finish after every active processor call
returns; a permanently blocked processor will therefore prevent shutdown from
completing.

## Metrics

`WorkerPipelineMetrics` contains:

| Metric | Meaning |
| --- | --- |
| `packets_submitted` | Submissions that returned `kAccepted` |
| `packets_processed` | Processor calls that returned normally |
| `packets_dropped_queue_full` | Submissions that returned `kQueueFull` |
| `packets_rejected_stopped` | Submissions that returned `kStopped` |
| `handler_failures` | Processor calls that threw an exception |
| `queue_high_watermark` | Largest observed number of packets waiting in the queue |

After `stop()` has drained the pipeline, this invariant holds:

```text
packets_submitted = packets_processed + handler_failures
```

The high-water mark never exceeds the configured queue capacity. A value close
to the capacity means the processor is nearly saturated even if no packet has
yet been dropped.

## Runtime integration

`Runtime::run` creates one worker pipeline for that run:

```text
receive valid packet
        |
        v
classify source sequence on I/O thread
        |
        v
increment Runtime packets_received
        |
        v
try_submit to WorkerPipeline
        |
        +-- accepted ------> SensorSampleProcessor on worker
        |
        +-- queue full ----> count and discard
```

When the stop predicate becomes true or `--count` is reached, Runtime stops
polling sockets and calls `WorkerPipeline::stop()`. `Runtime::run` returns only
after accepted work has drained.

Source sequence tracking happens before `try_submit`. It therefore preserves
receive order and records a valid packet even if the bounded queue later drops
that packet. Running the tracker inside workers would confuse worker scheduling
with network reordering. See
[sensor stream tracking](sensor-stream-tracking.md).

In the normal Runtime path, the pipeline's `PacketProcessor` is an internal
adapter that calls `SensorSampleProcessor::process`. Type-specific decoding
therefore stays off the I/O thread. An invalid typed payload returns normally
from the adapter and is distinguished by the sample rejection metric.

`--count` counts valid packets considered by Runtime, including packets
rejected because the worker queue was full or later rejected by typed payload
validation. This makes a bounded experiment terminate after the requested
amount of network input even under overload.

The final executable summary includes receiver, stream, pipeline, and sample
metrics:

```text
runtime stopped received=10 datagrams=10 accepted=10 rejected=0 \
epoll_wakeups=4 streams_observed=1 packets_observed=10 \
first_observations=1 in_order_observations=9 gap_observations=0 \
submitted=10 processed=10 dropped_queue_full=0 \
rejected_stopped=0 handler_failures=0 queue_high_watermark=3 \
packets_examined=10 samples_decoded=10 payloads_rejected=0
```

## Library use

The Runtime owns the pipeline in the normal application path:

```cpp
driveflow::runtime::RuntimeConfig config;
config.receiver.listen_endpoints = {
    {.address = "127.0.0.1", .port = 9000},
};
config.pipeline = {
    .worker_count = 4,
    .queue_capacity = 4096,
};

driveflow::runtime::Runtime runtime(config);
const auto summary = runtime.run(
    [] { return application_should_stop(); },
    [](const driveflow::runtime::SensorSample& sample) {
      process_sample(sample);
    });
```

See [sensor sample processing](sensor-sample-processing.md) for the typed seam.

`WorkerPipeline` can also be used directly when another producer already has
`ReceivedPacket` values:

```cpp
driveflow::runtime::WorkerPipeline pipeline(
    {.worker_count = 2, .queue_capacity = 1024},
    [](const driveflow::runtime::ReceivedPacket& packet) {
      process_packet(packet);
    });

for (auto& packet : packets) {
  const auto result = pipeline.try_submit(std::move(packet));
  if (result == driveflow::runtime::SubmitResult::kQueueFull) {
    report_overload();
  }
}

const auto metrics = pipeline.stop();
```

## Tuning guidance

Increase the worker count when processing is CPU-heavy and independent across
packets. Increasing it beyond the available CPU capacity can instead increase
context switching and contention.

Increase queue capacity to absorb short bursts. A larger queue does not fix a
sustained processing deficit; it only delays drops and increases the maximum
time a packet can wait before processing.

Watch these values together:

- `queue_high_watermark` approaching capacity indicates pressure;
- `packets_dropped_queue_full` proves the pipeline was overloaded;
- receiver rejection indicates malformed protocol input, not worker overload;
- handler failures indicate processing errors, not capacity loss.

## Scope boundaries

The worker pipeline deliberately does not provide:

- task priorities;
- dynamic worker scaling;
- blocking submission or retry;
- per-source ordering partitions;
- processor timeouts or cancellation;
- persistent queues;
- structured exception storage.

Those capabilities can be added at later seams without putting locks or worker
lifecycle logic back into the epoll receiver.
