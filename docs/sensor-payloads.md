# Sensor payload schemas

DriveFlow packet headers identify a message type. The payload bytes for that type use the schemas
below. Integer and IEEE-754 floating-point bit patterns are serialized in big-endian byte order.
Packet timestamps remain in the common packet header and are not duplicated in sensor payloads.

## IMU

An IMU payload is always 24 bytes and contains six binary32 values.

| Offset | Size | Field | Unit |
| ---: | ---: | --- | --- |
| 0 | 4 | acceleration x | m/s² |
| 4 | 4 | acceleration y | m/s² |
| 8 | 4 | acceleration z | m/s² |
| 12 | 4 | angular velocity x | rad/s |
| 16 | 4 | angular velocity y | rad/s |
| 20 | 4 | angular velocity z | rad/s |

All values must be finite.

## GNSS

A GNSS payload is always 32 bytes. Coordinates and altitude use binary64; accuracy values use
binary32.

| Offset | Size | Field | Valid range |
| ---: | ---: | --- | --- |
| 0 | 8 | latitude | `[-90, 90]` degrees |
| 8 | 8 | longitude | `[-180, 180]` degrees |
| 16 | 8 | altitude | finite metres |
| 24 | 4 | horizontal accuracy | finite and at least `0` metres |
| 28 | 4 | vertical accuracy | finite and at least `0` metres |

## CameraMeta

A CameraMeta payload has a 24-byte fixed prefix followed by optional bytes used to simulate camera
metadata load.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 8 | frame id | Per-camera frame counter |
| 8 | 4 | width | Image width in pixels; must be non-zero |
| 12 | 4 | height | Image height in pixels; must be non-zero |
| 16 | 4 | exposure time | Microseconds |
| 20 | 4 | extra data length | Number of bytes following the fixed prefix |
| 24 | variable | extra data | Opaque simulator load bytes |

The encoded length field must exactly match the remaining bytes. Extra data is limited so the
complete payload remains within the DriveFlow packet payload maximum.

## Validation behavior

`encode_sensor_payload()` rejects invalid in-memory values with `std::invalid_argument` or
`std::length_error`. `decode_sensor_payload()` treats bytes as untrusted input and returns a
specific `SensorPayloadError` without throwing for malformed payload contents.
