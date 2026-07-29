# UDP transport

DriveFlow's first transport module moves one complete encoded packet in one IPv4 UDP datagram. It
is intentionally blocking and single-threaded. The later epoll receiver will build on the same
socket ownership and endpoint concepts without being part of this module yet.

## Interface

`driveflow::net::UdpSocket` owns one Linux UDP file descriptor. It is move-only and closes the file
descriptor automatically when it leaves scope.

- `open()` creates an unbound sender socket.
- `bind_to(endpoint)` creates a socket bound to a local IPv4 address and port. Port `0` asks Linux
  to choose an available port.
- `send_to(bytes, endpoint)` sends exactly one datagram.
- `receive(maximum_size)` receives exactly one datagram and reports its source endpoint. It throws
  if the datagram did not fit in the configured buffer.
- `local_endpoint()` returns the address and port currently assigned to the socket.

Invalid IPv4 text and invalid size arguments raise standard exceptions. Linux system-call failures
raise `std::system_error` with the original `errno` value.

## Loopback demo

Build the debug preset, then start the one-shot receiver:

```bash
./build/debug/driveflow_udp_receiver 127.0.0.1 9000
```

In a second terminal, send one valid DriveFlow packet:

```bash
./build/debug/driveflow_udp_sender 127.0.0.1 9000
```

The sender encodes an IMU packet with the existing protocol module. The receiver accepts one UDP
datagram, validates and decodes it, prints the packet metadata, and exits.
