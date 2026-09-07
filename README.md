# Middleware Comparison for ADAS/SDV Communication — Master's Thesis

Proof-of-concept implementations and benchmarking of three in-vehicle
communication middlewares for Advanced Driver Assistance Systems (ADAS) /
Software-Defined Vehicle (SDV) use cases:

- **[OpenDDS](./opendds/)** — Data Distribution Service (DDS)
- **[vsomeip](./vsomeip/)** — SOME/IP (AUTOSAR-aligned)
- **[Zenoh](./zenoh/)** — Eclipse Zenoh

## Thesis context

The work first surveyed eight candidate protocols for in-vehicle,
Ethernet-based communication — SOME/IP, DDS, Zenoh, gRPC, MQTT, NATS,
Protobuf, and TSN — and scored them with a Multi-Criteria Decision
Analysis (MCDA) across latency, determinism, QoS support, reliability,
scalability, integration effort, ecosystem maturity, and footprint.
**DDS and SOME/IP formed the leading group, with Zenoh third**; these
three were carried forward for detailed implementation and benchmarking.
TSN and Protobuf remained relevant as a Layer-2 enabler and a
serialization format respectively, rather than standalone
application-layer stacks, so they were not implemented as separate PoCs.

## Proof-of-concept design

To keep the comparison fair, all three protocols implement the **same**
ADAS use case, data schema, timing constraints, and benchmarking
methodology — only the middleware-specific implementation differs.

**Scenario:** a front-sensing ADAS function fed by multiple radar
sensors, abstracted as seven nodes — one central **ADAS_ECU** (aggregates
and monitors radar data) and six radar nodes, **Radar_1**–**Radar_6**.
Radar nodes are data producers; the ADAS_ECU is the consumer/monitor,
except for the calibration flow which is bidirectional (request/response).

**Four communication streams, identical across all three protocols:**

| Stream | Period | Max E2E delay | Max jitter | Fault trigger |
|---|---|---|---|---|
| Heartbeat (HB) | 20 ms | 5 ms | 1 ms | 1 consecutive violation |
| Object list (OBJ) | 20 ms | 25 ms | 5 ms | 3 consecutive violations |
| Detection list (DET) | 20 ms | 25 ms | 5 ms | 3 consecutive violations |
| Calibration (CAL) | 1000 ms | — | — | — |

- **Heartbeat**: liveness/timing check per radar (timestamp, sequence counter, health code).
- **Object / Detection**: fixed-size arrays (20 entries/cycle) of tracked objects / raw detections.
- **Calibration**: ECU-initiated request/response/ack handshake, used to stress request-style traffic on top of the periodic pub/sub load and to measure per-hop latency (request→ACK, ACK→response, response→ECU-ACK).

The data schema is middleware-agnostic by design — the same field sets,
array sizes, and periods are realized as packed C structs (vsomeip),
IDL types (OpenDDS), and equivalent binary layouts (Zenoh, implemented in
Python), so any behavioral differences can be attributed to the
middleware rather than the application data model.

## Repository structure

```
.
├── opendds/    OpenDDS PoC — see opendds/README.md
├── vsomeip/    vsomeip PoC — see vsomeip/README.md
└── zenoh/      Zenoh PoC — see zenoh/README.md
```

## Status

- [x] OpenDDS PoC — source added, README documented
- [x] vsomeip PoC — source added, README documented
- [x] Zenoh PoC — source added, README documented

## Notes

The original development environment differs from the machine used to
prepare this repository, so build steps in each subfolder's README are
documented from source/config inspection and prior notes rather than
freshly re-verified on this machine.
