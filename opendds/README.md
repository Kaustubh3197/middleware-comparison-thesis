# OpenDDS Proof of Concept

## What this PoC does

[Describe the scenario: e.g. "A publisher/subscriber pair exchanging
[data type] over DDS, used to evaluate OpenDDS's API ergonomics and
[latency/throughput/whatever you measured]."]

## Dependencies

- OpenDDS: **[fill in version/commit — check dds/Version.h or the
  OpenDDS folder's CHANGELOG.md]**
- ACE/TAO: **[fill in version — check ACE_TAO/ACE/ace/Version.h]**
- CMake: [version]
- Compiler: [e.g. GCC 11 / MSVC 2019]

## Repository layout

```
opendds/
├── idl/      IDL interface definitions (compiled by opendds_idl/tao_idl at build time)
├── src/      Publisher/subscriber application source
└── config/   QoS and DCPS config files (e.g. rtps.ini)
```

## Setup

1. Build/install OpenDDS following the official guide:
   https://opendds.readthedocs.io/en/latest-release/devguide/getting_started.html
2. Set environment variables (OpenDDS ships a setenv script for this):
   ```
   export DDS_ROOT=/path/to/OpenDDS
   export ACE_ROOT=$DDS_ROOT/ACE_TAO/ACE
   export TAO_ROOT=$DDS_ROOT/ACE_TAO/TAO
   source $DDS_ROOT/setenv.sh
   ```
3. Build this PoC:
   ```
   mkdir build && cd build
   cmake ..
   make
   ```

## Notes on generated files

Files under `idl/` are compiled by `opendds_idl` and `tao_idl` into
TypeSupport, stub, and skeleton code at build time. These generated files
are intentionally not committed — they are OpenDDS-version-specific and
regenerate automatically during the build.

## Verification status

Build steps above are documented from the original project's build files
and were not re-verified on the machine used to prepare this repository.
