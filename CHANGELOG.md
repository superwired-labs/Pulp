\# Changelog



All notable changes to \*\*PULP\*\* (Precompressed Upstream Layer Pipeline) will be documented in this file.



\## \[1.0.0] - 2026-08-03



\### Added

\- \*\*Core Engine:\*\* High-throughput C logging library for Windows x64 with zero external dependencies.

\- \*\*Lossless Semantic Pre-compression:\*\* Dictionary-based deduplication for URLs and IP addresses combined with LZ4 compression.

\- \*\*Thread-Local Storage Architecture:\*\* Low-lock contention on the critical path (`Pulp\_Write`).

\- \*\*Temporal Evictionless Cache:\*\* Multi-level temporary caching strategy without active eviction.

\- \*\*SIMD/AVX2 Optimizations:\*\* Hardware-accelerated IP anonymization and cache probing.

\- \*\*LogReader Utility:\*\* Parallelized binary log decoder and exporter to text formats.

\- \*\*Live Telemetry:\*\* `Pulp\_GetStats()` API returning real-time throughput and memory metrics in JSON.

\- \*\*Dual Licensing:\*\* AGPLv3 Open Source license and Commercial License support.

