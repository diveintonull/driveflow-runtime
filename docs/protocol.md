# DriveFlow wire protocol v1

The packet envelope and typed sensor payloads are serialized separately. IMU, GNSS, and CameraMeta
payload layouts are defined in [Sensor payload schemas](sensor-payloads.md).

## Packet boundary

A packet is one fixed-size header followed by exactly `payload_length` bytes. The UDP transport puts
one complete DriveFlow packet in one UDP datagram; DriveFlow does not perform fragmentation or
reassembly.

## Header

All integer fields use big-endian (network) byte order. The serialized header is always 32 bytes and
does not depend on a C++ struct's padding or host byte order.

| Offset | Size | Field | v1 meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `DRFL` (`0x4452464c`) |
| 4 | 2 | `version` | `1` |
| 6 | 2 | `message_type` | `1=IMU`, `2=GNSS`, `3=CameraMeta` |
| 8 | 8 | `sequence_number` | Per-source monotonically increasing counter |
| 16 | 8 | `source_timestamp_ns` | Nanoseconds from Linux `CLOCK_MONOTONIC` |
| 24 | 4 | `payload_length` | Bytes after the header |
| 28 | 4 | `crc32` | CRC-32/ISO-HDLC value described below |

Protocol v1 does not encode an explicit source or source-session ID. Runtime
interprets the per-source sequence number using the remote endpoint, local
listener, and message type during one run. See
[sensor stream tracking](sensor-stream-tracking.md) for the exact identity,
wrap-around, and limitation semantics.

The maximum payload is 65,475 bytes, keeping the complete v1 packet within the maximum IPv4 UDP
payload of 65,507 bytes. Demo configurations should stay far below that limit to avoid IP
fragmentation.

## CRC

The checksum is CRC-32/ISO-HDLC (the common reflected CRC-32 with polynomial `0xedb88320`, initial
state `0xffffffff`, and final XOR `0xffffffff`). It covers header bytes `[0, 28)` followed by the
payload. The four-byte `crc32` field itself is excluded.

The Sensor Simulator's `--corrupt-every` option deliberately flips a packet bit
after this CRC has been encoded. It provides repeatable `kCrcMismatch` input
without weakening the normal encoder.

## Decoder behavior

The decoder accepts untrusted bytes and returns a specific error instead of throwing for malformed
input. Validation order is structural length, magic, version, type, size limit, exact packet length,
then CRC. This makes failures deterministic and keeps corrupted input from reaching later modules.
