# Changelog



All notable changes to **PULP** (Precompressed Upstream Layer Pipeline) will be documented in this file.



## [1.0.0] - 2026-08-03



### Added

- **Core Engine:** High-throughput C logging library for Windows x64 with zero external dependencies.

- **Lossless Semantic Pre-compression:** Dictionary-based deduplication for URLs and IP addresses combined with LZ4 compression.

- **Thread-Local Storage Architecture:** Low-lock contention on the critical path (`Pulp_Write`).

- **Temporal Evictionless Cache:** Multi-level temporary caching strategy without active eviction.

- **SIMD/AVX2 Optimizations:** Hardware-accelerated IP anonymization and cache probing.

- **LogReader Utility:** Parallelized binary log decoder and exporter to text formats.

- **Live Telemetry:** `Pulp_GetStats()` API returning real-time throughput and memory metrics in JSON.

- **Dual Licensing:** AGPLv3 Open Source license and Commercial License support.

## [1.0.0] - 2026-08-24
### Fixed
- IPv6 bracketed addresses with port now correctly anonymized (StripBrackets memchr)
- FastSeed() sign extension on non-ASCII characters
- Empty error_path now defaults to "error"
- General hardening and little fixes

### Changed
- IPv6 compressed notation masking limitation documented (RFC 5952)

