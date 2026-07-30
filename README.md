# DriveFlow Runtime

DriveFlow is a Linux C++20 runtime for receiving, validating, distributing, recording, and replaying
simulated vehicle sensor data. It focuses on explicit binary protocols, Linux I/O, bounded
concurrency, inter-process communication, deterministic replay, and measurable reliability.

## Documentation

- [Wire protocol and validation rules](docs/protocol.md)
- [Sensor payload schemas](docs/sensor-payloads.md)
- [UDP transport and loopback demo](docs/udp.md)

## Prerequisites

- Ubuntu 22.04 or 24.04
- GCC or Clang with C++20 support
- CMake 3.22+
- Ninja
- Git (used by CMake to fetch GoogleTest when it is not installed locally)

Optional developer tools are `clang-format` and `clang-tidy`. CMake exposes formatting targets only
when `clang-format` is installed; configure with `-DDRIVEFLOW_ENABLE_CLANG_TIDY=ON` to enable
`clang-tidy`.

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Sanitizer verification:

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

The TSan preset launches tests with `setarch -R`. This avoids the
`ThreadSanitizer: unexpected memory mapping` startup failure seen on some WSL2 kernels; it changes
only the launched process and does not alter system-wide ASLR settings.

Release build:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

If available:

```bash
cmake --build build/debug --target format-check
cmake --preset debug -DDRIVEFLOW_ENABLE_CLANG_TIDY=ON
cmake --build --preset debug
```

## Fuzz target

The decoder entry point can be built with Clang and libFuzzer:

```bash
CC=clang CXX=clang++ cmake --preset asan -DDRIVEFLOW_BUILD_FUZZERS=ON
cmake --build --preset asan --target decode_packet_fuzz
./build/asan/decode_packet_fuzz
```
