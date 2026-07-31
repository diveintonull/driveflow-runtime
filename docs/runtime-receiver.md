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
Runtime packet handler
```

The DriveFlow runtime receiver is the long-running network entry point for
sensor packets. It uses Linux non-blocking UDP sockets and one level-triggered
`epoll` instance to monitor one or more listen endpoints.

The receiver deliberately stops at the packet boundary. It validates the
DriveFlow packet envelope and CRC, but it does not decode IMU, GNSS, or camera
payloads. Payload decoding and downstream processing belong in the worker
pipeline.

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

## Command-line options

| Option | Meaning | Default |
| --- | --- | --- |
| `--listen <IPv4:port>` | UDP endpoint to bind; repeatable | `0.0.0.0:9000` |
| `--count <packets>` | Stop after delivering this many valid packets | continuous |
| `--poll-timeout-ms <ms>` | Maximum `epoll_wait` duration | 100 |
| `--max-drain <datagrams>` | Maximum datagrams drained from one ready socket per poll | 64 |
| `--help` | Print usage |  |

Ports, counts, timeouts, and drain limits must be positive. Singleton options
cannot be repeated. `--listen` is the only repeatable option.

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
invalid. This is intentional: payload decoding will run in the worker
pipeline, outside the I/O receive path.

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
| `delivered` | Valid packets handed to the runtime packet handler |
| `datagrams` | UDP datagrams read from all sockets |
| `accepted` | Datagrams that passed packet-level validation |
| `rejected` | Datagrams rejected by packet-level validation |
| `epoll_wakeups` | Poll calls that returned at least one ready descriptor |

A timeout is not counted as an `epoll` wakeup.

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
the flag, exits, and prints final metrics.

## Library interface

Applications can use the Runtime without the command-line executable:

```cpp
driveflow::runtime::RuntimeConfig config;
config.receiver.listen_endpoints = {
    {.address = "127.0.0.1", .port = 9000},
};
config.poll_timeout = std::chrono::milliseconds{100};

driveflow::runtime::Runtime runtime(config);
const auto summary = runtime.run(
    [] { return application_should_stop(); },
    [](const driveflow::runtime::ReceivedPacket& packet) {
      enqueue_for_workers(packet);
    });
```

The handler is the intended connection point for the bounded worker queue in
the next pipeline layer.

## Current boundaries

This receiver does not yet provide:

- worker threads or a bounded queue;
- type-specific sensor payload decoding;
- sequence-gap, duplicate, or reordering detection;
- source-health state;
- batching with `recvmmsg`;
- recording, replay, or shared-memory distribution.

Those capabilities can be added downstream without changing how sockets are
registered and drained.
