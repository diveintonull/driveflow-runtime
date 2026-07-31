# Sensor simulator

`driveflow_sensor_simulator` produces a continuous stream of valid DriveFlow packets over IPv4
UDP. One process represents one Sensor Source. Start separate processes for IMU, GNSS, and
CameraMeta streams.

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

Press `Ctrl+C` to stop a continuous run. `SIGINT` and `SIGTERM` request a graceful stop; rate waits
check that request every 10 ms.

## Options

| Option | Required | Default | Meaning |
| --- | --- | --- | --- |
| `--sensor <imu\|gnss\|camera>` | yes | none | Sensor Source type |
| `--destination <IPv4>` | no | `127.0.0.1` | UDP destination address |
| `--port <port>` | no | `9000` | UDP destination port |
| `--rate-hz <frequency>` | no | type-specific | Packet frequency |
| `--count <packets>` | no | continuous | Stop after a positive number of packets |
| `--camera-extra-bytes <bytes>` | no | `0` | CameraMeta-only opaque load bytes |
| `--help` | no | n/a | Print usage and exit |

The default rates are 200 Hz for IMU, 10 Hz for GNSS, and 30 Hz for CameraMeta. Frequencies must
be finite, greater than zero, and no more than 1,000,000 Hz. Camera extra data cannot exceed the
protocol payload limit.

## Packet behavior

- Sequence Numbers start at `1` for each simulator process and increase by one per packet.
- Source Timestamps come from `std::chrono::steady_clock` and use nanoseconds.
- Each Sensor Sample is encoded by the existing typed payload codec, then wrapped by the existing
  Packet codec, including CRC32.
- CameraMeta `frame_id` matches the packet Sequence Number.
- Camera extra bytes use a deterministic repeating `0..255` pattern.
- `--count` omits the final rate wait and exits immediately after the requested packet is sent.

The simulator intentionally does not inject loss, duplication, reordering, delay, or corruption
yet. Those controls belong to the later fault-injection stage and will build on this valid baseline
stream.
