# Zenoh Proof of Concept

## What this PoC does

[Describe the scenario: e.g. "A publisher/subscriber pair exchanging
[data type] over Zenoh, used to evaluate its API and
[latency/throughput/whatever you measured]."]

## Dependencies

- Zenoh: **[fill in version — check Cargo.toml if Rust, or the C/C++
  binding version if using zenoh-c/zenoh-cpp]**
- Language/toolchain: [e.g. Rust 1.7x, or zenoh-c + CMake if C++]

## Repository layout

```
zenoh/
├── src/      Publisher/subscriber application source
└── config/   Zenoh session/router config (if any custom config was used)
```

## Setup

[Fill in based on whether this was Rust-native zenoh or the C++ binding
(zenoh-cpp/zenoh-c), e.g.:]

```
# Rust
cargo build --release

# or zenoh-cpp / zenoh-c
mkdir build && cd build
cmake ..
make
```

## Verification status

Build steps above are documented from the original project's build files
and were not re-verified on the machine used to prepare this repository.
