# PULP Roadmap



This document outlines the planned, proposed, and candidate features for future releases of **PULP**. 

Priorities may adjust based on community feedback, sponsor requests, and contributor availability.



---


### High Priority / Short-Term

- [ ] **`pulpReader` Multi-Format CLI Exporter:** Extend the `pulpReader` CLI to stream decoded binary shards directly to stdout in **JSON**, **NDJSON**, **OTLP (OpenTelemetry)** and **Prometheus** formats (`pulpReader --input shard.pulp --format json|otlp`).
- [ ] **`pulpReader` In-Line Filtering:** Implement high-speed pre-decoding filtering (`--filter "http_code>=400"`) directly inside `pulpReader` to discard non-matching events before text/JSON serialization, saving CPU and RAM.
- [ ] **Explicit Force Flush API:** Expose `PulpForceFlush()` to enable application-driven or timer-driven buffer flushes on low-traffic streams or custom events without stopping the logger.
- [ ] **Rust Binding:** Safe Rust wrapper crate around `pulp.dll` published on crates.io.
- [ ] **C# / .NET Integration (`Pulp.Net`):** Native P/Invoke wrapper and NuGet package for seamless integration into ASP.NET Core and IIS workloads.
- [ ] **NDR / IPFIX Variant:** Dedicated 64-byte payload format (dual IPv4/v6, free-form 64-bit metrics) for high-throughput network monitoring and traffic analysis.



---


### Medium Priority

- [ ] **`libpulpreader` C API & Shared Library:** Expose a lightweight standalone decoding C API (`libpulpreader.dll` / `.so`) to enable FFI bindings (Python, Go, Rust) and custom in-process ingestion pipelines.
- [ ] **Fluent Bit, Vector & OTLP Exporters:** Provide native bridge configs and streaming exporters to feed converted `.pulp` archives directly into OpenTelemetry collectors, Fluent Bit, Vector, and SIEM endpoints.
- [ ] **C++ Header-Only Wrapper (`pulp.hpp`):** Modern RAII wrapper for high-frequency trading and C++ game server architectures.
- [ ] **Enhanced Integration Examples:** Complete sample projects for IIS native C++ modules and C# / .NET integrations.

---


### Long-Term / Enterprise Features

- [ ] **Linux Kernel/POSIX Port:** POSIX reimplementation for Linux server environments leveraging the kernel's unique features.
- [ ] **Multi-Socket NUMA Optimizations:** Lock-free inter-process allocations for hyper-scale (>64 core) architectures.
- [ ] **Queryable archives** Selective indexing layer for forensic analysis.

---


### Ideas & Community Proposals

Have an idea or want to contribute? Feel free to open a [GitHub Issue](https://github.com/superwired-labs/Pulp/issues) or start a discussion!

