# vsomeip Prototype

Part of a three-way middleware comparison (OpenDDS / vsomeip / Zenoh) for
ADAS in-vehicle communication — see the [top-level README](../README.md)
for full thesis context, the shared ADAS scenario, and the four
communication streams (Heartbeat, Object, Detection, Calibration) that
all three prototypes implement identically.

## Software architecture

Two roles, matching the SOME/IP service model:

- **Radar node (SOME/IP server)**: offers a service (`OfferService` via
  UDP multicast) covering the Heartbeat/Object/Detection eventgroup, and
  responds to calibration method calls.
- **ADAS_ECU (SOME/IP client)**: subscribes to the eventgroup
  (`SubscribeEventgroup` → `SubscribeEventgroupAck`), receives periodic
  event notifications, and issues calibration as a SOME/IP method call
  (`CalibrationRequest` → `CalibrationResponse`).

This mirrors SOME/IP's native split between **events** (used for the
periodic Heartbeat/Object/Detection streams — publish-style, matching
the eventgroup subscription model) and **methods** (used for
Calibration — a real request/response RPC, which vsomeip supports
directly, unlike OpenDDS/Zenoh where request/response has to be built
on top of plain pub/sub topics).

## Calibration state machine

The calibration flow is implemented as a small state machine on both sides:

- **Radar side** — one state per radar: `Idle` → `RequestReceived`
  (on incoming calibration event) → `AckSent` (internal timer fires the
  acknowledgement method call back to the ECU) → `ResponseSent` (final
  response) → back to `Idle`. All transitions and timestamps are logged.
- **ECU side** — one context per radar, keyed by a calibration
  identifier: broadcasting a request moves the context to
  `WaitingForAck` (send time stored); receiving the ACK method moves it
  to `WaitingForResponse`; receiving the final response returns it to
  `Idle`.

This state machine doesn't model a real calibration algorithm — its
purpose is to stress SOME/IP methods and events under mixed traffic and
produce measurable end-to-end timing behavior, consistent with the
Calibration stream's role in the other two prototypes.

## Repository layout

```
vsomeip/
├── src/      Radar (server) and ADAS_ECU (client) application source
└── config/   vsomeip JSON configuration (service/instance/eventgroup IDs, ports, routing)
```

## Dependencies

- vsomeip: *[fill in version/commit — check `CMakeLists.txt` or the vsomeip source tree]*
- Boost (vsomeip depends on it — check which components: system, thread, etc.)
- CMake, and a C++ compiler with C++11 or later

## Setup

1. Build/install vsomeip following the official guide:
   https://github.com/COVESA/vsomeip
2. Point the application at its config and application name via
   environment variables, per vsomeip's convention:
   ```bash
   export VSOMEIP_CONFIGURATION=./config/vsomeip.json
   export VSOMEIP_APPLICATION_NAME=[app name from your config]
   ```
3. Build this PoC:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
4. Run the radar (server) side(s) and the ADAS_ECU (client) side per
   your config's routing setup.

## Verification status

Build/run steps above are documented from the project's CMake/config
files and were not re-verified on the machine used to prepare this
repository (development and benchmarking were carried out on a
different machine).
