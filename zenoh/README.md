# Zenoh Prototype

Part of a three-way middleware comparison (OpenDDS / vsomeip / Zenoh) for
ADAS in-vehicle communication — see the [top-level README](../README.md)
for full thesis context, the shared ADAS scenario, and the four
communication streams (Heartbeat, Object, Detection, Calibration) that
all three prototypes implement identically.

## Software architecture

Implemented in Python, over Zenoh running on TCP. Like OpenDDS, Zenoh
offers only topic-based (key-expression) pub/sub — there is no native
request/response primitive — so the Calibration flow is implemented
manually as a set of separate request/response/ack keys that the
application code matches up itself, the same pattern used in the
OpenDDS prototype (this is a notable ergonomic difference from vsomeip,
which supports request/response as a first-class method call).

## Key expression mapping

| Stream | Key expression(s) | Role |
|---|---|---|
| Heartbeat | `adas/heartbeat/radar/<id>` | Periodic liveness/timing check |
| Object list | `adas/objects/radar/<id>` | Reliable tracked-object stream |
| Detection list | `adas/detections/radar/<id>` | Reliable raw-detection stream |
| Calibration | `adas/calib/request` (broadcast), `adas/calib/ack/radar/<id>`, `adas/calib/response/<id>`, `adas/calib/ack/ecu/<id>`, `adas/calib/ready/<id>` | Broadcast request, per-radar ACK/response |

**Calibration flow** (conceptually identical to the OpenDDS and vsomeip
prototypes): each radar periodically publishes a small "ready" beacon.
The ECU broadcasts a calibration request every 1 s (random identifier +
three additional fields). On receipt, a radar immediately publishes an
acknowledgement (echoing the identifier, annotated with its local
receive timestamp), then after a random processing delay within a
configured range publishes a final response with synthetic calibration
data. The ECU acknowledges receipt back to the radar and records the
corresponding timestamps.

## Repository layout

```
zenoh/
├── src/      Radar and ADAS_ECU application source (Python)
└── config/   Zenoh session/router configuration (if any custom config was used)
```

## Dependencies

- Zenoh: *[fill in version — check `pip show eclipse-zenoh` or your requirements file]*
- Python 3.x
- `eclipse-zenoh` Python bindings

## Setup

1. Install the Zenoh Python API:
   ```bash
   pip install eclipse-zenoh
   ```
2. Run the radar-side and ECU-side scripts (see `src/`), configured for
   Zenoh over TCP as used in the benchmarking runs.

## Verification status

Setup steps above are documented from the project's source/config files
and were not re-verified on the machine used to prepare this repository
(development and benchmarking were carried out on a different machine).
