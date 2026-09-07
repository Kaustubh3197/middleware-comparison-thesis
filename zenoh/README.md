# Zenoh Prototype

Part of a three-way middleware comparison (OpenDDS / vsomeip / Zenoh) for
ADAS in-vehicle communication — see the [top-level README](../README.md)
for full thesis context, the shared ADAS scenario, and the four
communication streams (Heartbeat, Object, Detection, Calibration) that
all three prototypes implement identically.

## Software architecture

Implemented in Python using the official `zenoh` (Eclipse Zenoh) Python
bindings, running **peer-to-peer over TCP** — each process opens its own
Zenoh session with explicit `listen`/`connect` TCP endpoints
(`tcp/<ip>:<port>`, default port `7447`), so no separate `zenohd` router
process is required for this setup.

- **`radar.py`** — one process per radar (launched once per `--radar_id`
  by `run.sh`). Declares publishers for its Heartbeat/Object/Detection
  keys plus its calibration response/ack/ready keys, and a subscriber
  for the broadcast calibration request. Publishers are configured
  `RELIABLE`, `BLOCK` congestion control, `REAL_TIME` priority.
- **`adas_ecu.py`** — the ECU side: subscribes to every radar's streams
  and calibration topics, runs the 1 Hz calibration sender, aggregates
  per-radar/per-stream statistics (E2E delay, jitter, violations,
  faults), and writes a report at the end of the run (`.xlsx` via
  `xlsxwriter` if available, otherwise a set of `.csv` files).
- **`common.py`** — shared struct pack/unpack helpers (matching the same
  binary layout as the OpenDDS/vsomeip payloads), timing constants, and
  key-expression builders.
- **`run.sh`** — launches the ECU first, waits ~1.5 s, then starts
  `RADAR_COUNT` radar processes, waits for the ECU to finish, and opens
  the resulting report.

There is no static Zenoh config file — session config (listen/connect
endpoints) is built programmatically in `common`/`radar.py`/`adas_ecu.py`
from CLI args and environment variables, so `config/` is intentionally
empty in this PoC.

## Key expression mapping

| Stream | Key expression(s) | Role |
|---|---|---|
| Heartbeat | `adas/heartbeat/radar/<id>` | Periodic liveness/timing check |
| Object list | `adas/objects/radar/<id>` | Reliable tracked-object stream |
| Detection list | `adas/detections/radar/<id>` | Reliable raw-detection stream |
| Calibration | `adas/calib/request` (broadcast), `adas/calib/ack/radar/<id>`, `adas/calib/response/<id>`, `adas/calib/ack/ecu/<id>`, `adas/calib/ready/<id>` | Broadcast request, per-radar ACK/response |

**Calibration flow** (mirrors the OpenDDS and vsomeip prototypes): each
radar periodically publishes a "ready" beacon. The ECU broadcasts a
calibration request at 1 Hz (random request ID + 3 additional fields,
big-endian `>IIII`). A radar receiving it immediately acknowledges
(echoing the ID, annotated with its local receive timestamp), then
after a random processing delay (`CALIB_MS_MIN`–`CALIB_MS_MAX`,
default 80–220 ms) publishes a final response with synthetic
calibration data. The ECU acknowledges back to the radar and logs the
timestamps needed for the L1/L2/L3 latency breakdown.

## Repository layout

```
zenoh/
├── run.sh          Launches ECU + N radar processes, collects the report
├── src/
│   ├── adas_ecu.py     ECU side — aggregation, calibration sender, report writer
│   ├── radar.py        Radar side — HB/OBJ/DET publishers + calibration responder
│   └── common.py       Shared pack/unpack structs, timing constants, key builders
└── config/         (unused — session config is built from CLI args/env vars, not a file)
```

## Dependencies

- Python 3.x
- `eclipse-zenoh` (imported as `zenoh`) — Python bindings
- `xlsxwriter` — optional, for `.xlsx` report output (falls back to CSV if not installed)

## Setup

1. Install dependencies:
   ```bash
   pip install eclipse-zenoh xlsxwriter
   ```
2. Run a full scenario (ECU + N radars) via the launcher script:
   ```bash
   RADAR_COUNT=6 RUN_FOR_S=60 ./run.sh
   ```
   Useful environment variables (see `run.sh` for the full list and
   defaults): `ADAS_IP` (default `127.0.0.10`), `PORT` (default `7447`),
   `RADAR_COUNT`, `RUN_FOR_S`, and the calibration knobs `CALIB`,
   `CALIB_DURATION_S`, `CALIB_DRAIN_S`, `CALIB_WARMUP_S`,
   `CALIB_READY_PERIOD`, `CALIB_MS_MIN`/`CALIB_MS_MAX`.
3. Output (logs + `.xlsx`/`.csv` report) is written to
   `output/<timestamp>/`, which is excluded from git — each run
   regenerates its own report locally.

## Verification status

Setup steps above are documented from the project's actual `run.sh`,
`radar.py`, and `adas_ecu.py` and were not re-verified on the machine
used to prepare this repository (development and benchmarking were
carried out on a different machine).
