# vsomeip Prototype

Part of a three-way middleware comparison (OpenDDS / vsomeip / Zenoh) for
ADAS in-vehicle communication — see the [top-level README](../README.md)
for full thesis context, the shared ADAS scenario, and the four
communication streams (Heartbeat, Object, Detection, Calibration) that
all three prototypes implement identically.

## Software architecture

Two C++17 executables, built on the official [vsomeip](https://github.com/COVESA/vsomeip)
library (originally developed by BMW), each structured into three layers:

1. **Configuration and bootstrap** — creates the vsomeip application,
   loads the JSON configuration (services, instances, events,
   transports), and registers callbacks for incoming events/methods.
2. **Data generation and timing** — maintains per-radar state (sequence
   counters, synthetic object/detection lists) and generates new
   payloads at the configured cycle times, using a monotonic clock for
   all delay/jitter timestamps.
3. **vsomeip integration** — serializes internal data structures into
   SOME/IP payloads, publishes events, calls methods, and processes
   vsomeip runtime callbacks.

The vsomeip runtime itself handles sockets, marshalling, message
routing, and service discovery, so application code only deals with
typed structures and named functions.

- **`radar_publisher`** (app id `0x0101`): one binary handling all 6
  radar instances (selected via `--radar-ids`/`--slots`-style args);
  offers the Heartbeat/Object/Detection events and the Calibration
  methods/event per instance.
- **`adas_subscriber`** (app id `0x0100`): the SOME/IP **routing
  manager** for this setup (`"routing": "adas_subscriber"` in the
  config) and the ECU client — subscribes to all radar eventgroups and
  calls the calibration methods. Built with `USE_XLSX` to additionally
  export logs as `.xlsx` via libxlsxwriter.

## Service / event / transport mapping

Each of the 6 logical radars is a separate SOME/IP **instance** under a
shared **service** ID per stream — the ECU tells radars apart purely by
instance ID, matching the thesis design (Table 4.1):

| Stream | SOME/IP primitive | Transport | Service | Instances (Radar 1–6) | Event/Method IDs | Eventgroup |
|---|---|---|---|---|---|---|
| Heartbeat | Event | UDP (`unreliable`) | `0x1234` | `0x5678`–`0x567D` | event `0x8778` | `0x4465` |
| Object list | Event | TCP (`reliable`) | `0x2345` | `0x6789`–`0x678E` | event `0x9889` | `0x5566` |
| Detection list | Event | TCP (`reliable`) | `0x3456` | `0x7890`–`0x7895` | event `0xAABB` | `0x6677` |
| Calibration | Methods + Event | TCP (`reliable`) | `0x4567` | `0x8A00`–`0x8A05` | event `0xCA01`, methods `0xCA02`/`0xCA03` | `0xCA1B` |

Ports: Heartbeat/Object/Detection share `30508` (reliable) / `30509`
(unreliable); Object list additionally listens on `30510`; Detection on
`30511`; Calibration on `30512`. Service discovery runs on UDP multicast
`224.0.0.1:30490`.

This intentionally mirrors a simple QoS split: Heartbeat is a
lightweight, best-effort UDP event, while Object/Detection/Calibration
use reliable TCP-based events or methods — vsomeip guarantees ordered,
retransmitted delivery for those. The ECU application layers its own
timeout/fault logic on top (per-radar, per-stream deadlines, fault
counters, summary logs).

## Calibration state machine

The Calibration use case (`method 0xCA02`/`0xCA03` + `event 0xCA01`)
runs a small state machine on both sides — see the
[top-level README](../README.md) for the shared timing constraints:

- **Radar side**: `Idle` → `RequestReceived` (calibration event in) →
  `AckSent` (internal timer fires the ACK method call back to the ECU)
  → `ResponseSent` (final response) → back to `Idle`. All transitions
  and timestamps are logged.
- **ECU side**: one context per radar keyed by calibration identifier —
  broadcasting a request moves it to `WaitingForAck` (send time
  stored); receiving the ACK method moves it to `WaitingForResponse`;
  the final response returns it to `Idle`.

This isn't meant to emulate a real calibration algorithm — its purpose
is to stress SOME/IP methods and events under mixed traffic and produce
measurable end-to-end timing behavior.

## Repository layout

```
vsomeip/
├── CMakeLists.txt
├── src/
│   ├── radar_publisher.cpp   Radar side — offers all 4 streams per instance
│   └── adas_subscriber.cpp   ECU side — routing manager + client, .xlsx export
└── config/
    └── vsomeip.json          Services/instances/events/methods/service-discovery config
```

## Dependencies

- vsomeip3 (or vsomeip) — headers (`vsomeip/vsomeip.hpp`) and library,
  found via `find_path`/`find_library` (no pinned version in the
  build — install `libvsomeip3-dev` or build vsomeip from source)
- Boost — `system`, `thread`, `log` components
- pthreads (`Threads::Threads`)
- zlib + **libxlsxwriter** — used by `adas_subscriber` (`USE_XLSX`) for
  `.xlsx` log export, same as the OpenDDS ECU side
- CMake 3.10+, C++17 compiler (GCC < 9 additionally needs `stdc++fs`)

## Setup

1. Install vsomeip (via `libvsomeip3-dev` on Debian/Ubuntu, or build
   from source: https://github.com/COVESA/vsomeip), Boost, and
   libxlsxwriter:
   ```bash
   sudo apt install libboost-all-dev libxlsxwriter-dev zlib1g-dev
   ```
2. Build:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
3. Point each process at the shared config and its own application
   name/id (both are already declared in `vsomeip.json`):
   ```bash
   export VSOMEIP_CONFIGURATION=./config/vsomeip.json
   export VSOMEIP_APPLICATION_NAME=adas_subscriber   # or radar_publisher
   ```
4. Run `adas_subscriber` first (it's the configured routing manager),
   then `radar_publisher` for the radar instances.

## Verification status

Build/run steps above are documented from the project's actual
`CMakeLists.txt` and `vsomeip.json` and were not re-verified on the
machine used to prepare this repository (development and benchmarking
were carried out on a different machine).
