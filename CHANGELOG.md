# Changelog



All notable changes to **PULP** (Precompressed Upstream Layer Pipeline) will be documented in this file.



## [1.0.0] - 2026-08-03



### Added

- **Core Engine:** High-throughput C logging library for Windows x64 with zero external dependencies.

- **Lossless Semantic Pre-compression:** Dictionary-based deduplication for URLs and IP addresses combined with LZ4 compression.

- **Thread-Local Storage Architecture:** Low-lock contention on the critical path (`Pulp_Write`).

- **Temporal Evictionless Cache:** Multi-level temporary caching strategy without active eviction.

- **SIMD/AVX2 Optimizations:** Hardware-accelerated IP anonymization and cache probing.

- **PulpReader Utility:** Parallelized binary log decoder and exporter to text formats.

- **Live Telemetry:** `Pulp_GetStats()` API returning real-time throughput and memory metrics in JSON.

- **Dual Licensing:** AGPLv3 Open Source license and Commercial License support.

2026-08-24
### Fixed
- IPv6 bracketed addresses with ports now correctly anonymized via memchr() in StripBrackets()
- FastSeed() sign extension on non-ASCII characters (cast unsigned char)
- Empty error_path now defaults to "error" in PulpInit()
- IP buffer clamp unified to MAX_IPV6_LEN (79) across all code paths
- AnonIp() NULL return now checked with bounded fallback copy
- Infinite loop in URL cache probing resolved (uint16_t → size_t)
- Deadlock in PulpFlush() when CreateThreadpoolWork fails (signal pc_complete added)
- WriteFile now uses exclusive SRWLock instead of shared lock
- Double-checked locking implemented for file_handle creation
- head/tail queue indices declared volatile
- errorlock CriticalSection initialized at top of InitializeOnce() before any WriteError() call
- WritePool_Init() returns BOOL, checked by caller
- WritePool_Init() rollback symmetry corrected (j < i, CloseHandle instead of free)
- Empty backup_path ("") now skipped instead of failing
- herror handle now closed in PulpShutdown()
- TLS dangling pointer guarded (tls_index != TLS_OUT_OF_INDEXES check before TlsSetValue)
- g_max_pending_handles forced to power of 2 for correct ring buffer masking
- IPv6 "::" expansion now correctly computes missing hextets count
- IPv6 Zone IDs (%eth0) supported in ClassifyIpFast()

### Changed
- IPv6 compressed notation masking limitation documented (RFC 5952 by design)
- Experimental note added for LZ4 configurations exceeding 2GB limit
- Documentation aligned with code (char limits, default values)
- Misleading IPv4 zone ID reference removed from docs

### Security
- IPv6 bracketed addresses (RFC 3986) now properly anonymized (RGPD compliance fix)
- Sign extension in FastSeed() eliminated for correct hash distribution

