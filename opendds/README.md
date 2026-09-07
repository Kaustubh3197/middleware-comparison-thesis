# OpenDDS Prototype

Part of a three-way middleware comparison (OpenDDS / vsomeip / Zenoh) for
ADAS in-vehicle communication — see the [top-level README](../README.md)
for full thesis context, the shared ADAS scenario, and the four
communication streams (Heartbeat, Object, Detection, Calibration) that
all three prototypes implement identically.

## Software architecture

Two C++ executables, joining the same DDS domain:

- **`radar_publisher`** (radar side): publishes Heartbeat, Object list,
  and Detection list topics; subscribes to calibration requests and ECU
  acknowledgements; sends calibration acknowledgements, responses, and
  "ready" beacons.
- **`ecu_subscriber`** (ECU side): subscribes to all radar streams and
  calibration topics; publishes calibration requests and ECU
  acknowledgements. Uses a DDS wait-set with read conditions to handle
  all subscriptions in a single main loop.

Both use OpenDDS for discovery, topic matching, and RTPS/UDP transport.
All data types are defined once in a shared IDL file (`idl/AdasZenohCompat.idl`),
so application code works with strongly typed C++ structs such as
`Adas::Heartbeat`, `Adas::ObjectsMsg`, and `Adas::DetectionsMsg`.

On the radar side, each calibration request is handled in a short-lived
worker thread that emulates processing time before sending the final
response. On the ECU side, a dedicated calibration sender runs at 1 Hz,
starting once the first radar data arrives. Send/receive timestamps are
taken from a monotonic clock and logged, forming the basis for the
delay/jitter measurements.

## Topic mapping

| Stream | DDS topic(s) | Transport | Role |
|---|---|---|---|
| Heartbeat | `adas/heartbeat` | RTPS/UDP | Periodic liveness/timing check |
| Object list | `adas/objects` | RTPS/UDP | Tracked-object list per cycle |
| Detection list | `adas/detections` | RTPS/UDP | Raw-detection list per cycle |
| Calibration | `adas/calib/request`, `adas/calib/ack/radar`, `adas/calib/response`, `adas/calib/ack/ecu`, `adas/calib/ready` | RTPS/UDP | Request/ACK/response handshake + readiness beacons |

**Calibration flow:** each radar periodically publishes a `ready` beacon.
The ECU sends a calibration request every 1 s (random ID + a few
traceability fields, broadcast or addressed to a single radar). A radar
receiving a matching request immediately acknowledges (echoing the ID +
its local receive time), then after a configurable processing delay
publishes a response with synthetic calibration data. The ECU
acknowledges receipt on the ECU-ACK topic. Per request, four timestamps
(ECU send, radar ACK, radar response, ECU-ACK) yield three latency
components: L1 (ECU→radar request-ACK), L2 (radar processing,
ACK-response), and L3 (radar→ECU response-ACK).

## Output / logging

Each completed calibration transaction is logged as one line in a text
log (ECU send, radar ACK, radar response, ECU-ACK timestamps, and the
derived L1/L2/L3 latencies). When built with `USE_XLSXWRITER=ON`
(the default), `ecu_subscriber` additionally writes these as rows in an
Excel calibration worksheet for easier post-processing.

## Repository layout

```
opendds/
├── idl/      Shared ADAS data-type definitions (AdasZenohCompat.idl)
├── src/      radar_publisher.cpp, ecu_subscriber.cpp
├── config/   DDS/QoS configuration
├── rtps.ini  RTPS/UDP transport configuration
└── run_in_terminals.sh   Convenience script to launch both sides
```

## Dependencies

- OpenDDS with CMake package support (`find_package(OpenDDS)`,
  providing `OpenDDS::Dcps` and `opendds_target_sources()`) —
  version: *[fill in — check `dds/Version.h` in your OpenDDS install, or `DDS_ROOT/VERSION.txt`]*
- ACE/TAO (bundled with the OpenDDS build): *[fill in — check `ACE_ROOT/ace/Version.h`]*
- CMake 3.16+
- C++ compiler with C++11 support
- **libxlsxwriter** + **zlib** — optional, used only by `ecu_subscriber`
  to export calibration logs as `.xlsx` (see `USE_XLSXWRITER` below)

## Setup

1. Build/install OpenDDS (with CMake integration) following the official guide:
   https://opendds.readthedocs.io/en/latest-release/devguide/getting_started.html
2. Set environment variables (OpenDDS ships a `setenv` script for this)
   and make sure OpenDDS's CMake config is discoverable
   (`CMAKE_PREFIX_PATH` pointing at your OpenDDS build, or the `setenv`
   script exporting it for you):
   ```bash
   export DDS_ROOT=/path/to/OpenDDS
   export ACE_ROOT=$DDS_ROOT/ACE_TAO/ACE
   export TAO_ROOT=$DDS_ROOT/ACE_TAO/TAO
   source $DDS_ROOT/setenv.sh
   ```
3. (Optional) Install libxlsxwriter and zlib if you want Excel log
   export from `ecu_subscriber` (on by default — see below to disable):
   ```bash
   # Debian/Ubuntu example
   sudo apt install libxlsxwriter-dev zlib1g-dev
   ```
4. Build this PoC:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
   To build without the Excel export dependency:
   ```bash
   cmake -DUSE_XLSXWRITER=OFF ..
   ```
5. Run both sides (see `run_in_terminals.sh` for the exact commands used
   during experiments — typically a `DCPSInfoRepo` discovery process,
   then `radar_publisher` per radar node and `ecu_subscriber` on the ECU side).

## Notes on generated/runtime files

- Files under `idl/` are compiled by `opendds_idl`/`tao_idl` into
  TypeSupport, stub, and skeleton C++ at build time — these generated
  files are not committed; they regenerate automatically and are
  OpenDDS-version-specific.
- `repo.ior` (written by `DCPSInfoRepo` at runtime, encoding the discovery
  service's live network address) is not committed — it's regenerated
  each time `DCPSInfoRepo` starts.

## Verification status

Build steps above are documented from the project's CMake/config files
and were not re-verified on the machine used to prepare this repository
(development and benchmarking were carried out on a different machine).
