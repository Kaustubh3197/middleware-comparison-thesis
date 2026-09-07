# vsomeip Proof of Concept

## What this PoC does

[Describe the scenario: e.g. "A client/service pair exchanging
[data/RPC] over SOME/IP, used to evaluate vsomeip's API and
[latency/throughput/whatever you measured]."]

## Dependencies

- vsomeip: **[fill in version/commit — check CMakeLists.txt or a
  vsomeip/version.hpp-style file in the vsomeip source tree]**
- Boost: [version, vsomeip depends on Boost — check which libs: system, thread, etc.]
- CMake: [version]
- Compiler: [e.g. GCC 11]

## Repository layout

```
vsomeip/
├── src/      Client/service application source
└── config/   vsomeip JSON configuration files (service IDs, ports, routing)
```

## Setup

1. Build/install vsomeip following the official guide:
   https://github.com/COVESA/vsomeip
2. Point the application at the config file via the environment variable
   vsomeip expects:
   ```
   export VSOMEIP_CONFIGURATION=./config/vsomeip.json
   export VSOMEIP_APPLICATION_NAME=[your app name]
   ```
3. Build this PoC:
   ```
   mkdir build && cd build
   cmake ..
   make
   ```

## Verification status

Build steps above are documented from the original project's build files
and were not re-verified on the machine used to prepare this repository.
