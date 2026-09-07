# Middleware Comparison — Master's Thesis

Proof-of-concept implementations and evaluation of three inter-process
communication / middleware protocols for [your thesis topic, e.g.
automotive/distributed systems communication]:

- **[OpenDDS](./opendds/)** — Data Distribution Service (DDS) implementation
- **[vsomeip](./vsomeip/)** — SOME/IP implementation (used in AUTOSAR/automotive contexts)
- **[Zenoh](./zenoh/)** — Zero-overhead pub/sub/query protocol

Each folder is self-contained with its own README covering the exact
dependency versions used, setup steps, and how to build/run that
protocol's PoC. This top-level README covers what's being compared and why.

## Thesis context

[Short description: what question the thesis is answering, e.g. "This work
evaluates latency, throughput, and API ergonomics of OpenDDS, vsomeip, and
Zenoh for real-time data distribution in constrained environments."]

## Repository structure

```
.
├── opendds/    OpenDDS PoC — see opendds/README.md
├── vsomeip/    vsomeip PoC — see vsomeip/README.md
└── zenoh/      Zenoh PoC — see zenoh/README.md
```

## Status

- [ ] OpenDDS PoC — documented / in progress
- [ ] vsomeip PoC — documented / in progress
- [ ] Zenoh PoC — documented / in progress

## Notes

The original development environment differs from the machine used to
prepare this repository, so build steps below are documented from source
inspection and prior notes rather than freshly verified on this machine.
See each subfolder's README for specifics.
