# PULP – Precompressed Upstream Layer Pipeline  

Copyright François Gauthier - Superwired-Labs

[![License](https://img.shields.io/badge/License-AGPL%20v3%20%2F%20Commercial-blue.svg)](LICENSES/AGPL-3.0.txt)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)]()
[![MSVC C11 Build Check](https://github.com/superwired-labs/Pulp/actions/workflows/build.yml/badge.svg)](https://github.com/superwired-labs/Pulp/actions/workflows/build.yml)

**Stop feeding raw logs to expensive storage/network.**

## Purpose & Scope

PULP is a native C library for Windows x64 that compresses structured data **at the source**, inside your own process, before it ever hits disk or network. Lossless semantic deduplication paired with LZ4 shrinks data 3–6×; IP anonymisation (AVX‑2) is applied inline; and a zero-allocation hot path sustains 20M+ logs/sec with fully deterministic memory.

Link the DLL, call one function per log, and let compressed binary shards accumulate. A companion CLI (`PulpReader`) decodes them back to text whenever you need.

**Key numbers** (6‑core Ryzen 5 Pro 8640HS, NVMe SSD):
- **+20 million logs/second** sustained throughput (including disk write, compression, optional anonymisation & parameter stripping)
- **3–6× compression** on realistic structured logs (entropy 3,6–4,4 bits/byte)
- **As low as 20 MB** memory footprint, fully deterministic under any load

### What is Included in PULP's Measured Throughput (20M+ logs/sec)?

PULP's benchmark throughput measures the complete end-to-end pipeline operating on realistic workloads:

| Operation | Executed Inline by PULP | Typically Skipped in Competitor Benchmarks |
| :--- | :---: | :---: |
| **Disk Persistence** (NVMe binary write) | ✅ | Replaced by `/dev/null` or RAM buffer |
| **Semantic Deduplication** (Persistent Dictionary) | ✅ | Not available in upstream loggers |
| **LZ4 Stream Compression** | ✅ | Disabled or replaced by lighter algorithms |
| **IP Anonymisation** (AVX2 vectorised) | ✅ | Requires separate downstream pipeline |
| **URL Parameter Stripping** | ✅ | Requires regex parsing downstream |
| **Automated File Rotation** (Handle Swap) | ✅ | Omitted from throughput calculations |
| **High-Cardinality Stress** (5M+ unique values) | ✅ | Measured using low-cardinality fixed strings |
| **Atomic Inter-Thread Sequencing** | ✅ | Omitted or un-sequenced |

> **Summary:** While standard tools measure how fast they can discard or process logs in memory, PULP processes, sanitises, compresses, and writes full-fidelity logs to physical disk in a single pass.

**Verify the numbers yourself**: `LogProducer` ships with embedded test datasets. Drop the DLL into the folder, run it on your hardware, results in under 5 minutes.

---

## When to use PULP

| Your situation | What PULP brings |
|----------------|-------------------|
| You build a high‑traffic web server, proxy, or firewall on Windows and need to log millions of requests per second. | PULP compresses on the fly and writes directly to disk, keeping CPU and memory usage predictable. |
| You want to reduce the cost of log storage and network egress. | Lossless semantic pre‑compression plus LZ4 typically shrinks structured data by a factor of 3–5×. |
| You are legally required to anonymise IP addresses before storing logs. | On-the-fly AVX‑2 accelerated masking works on both IPv4 and IPv6, with configurable levels. |
| You need an audit‑proof, corruption‑resiliant binary archive for compliance. | PULP batches are self‑contained and can be decoded years later with the supplied command‑line tool. |
| You already have a log collector (Fluent Bit, Vector, etc.) and just want a faster writer. | PULP outputs compressed `.bin` files that any collector can ship; you decide when and how to move them. |
| You don't want to manage a separate logging service or daemon. | PULP is a single DLL, linked statically or dynamically into your own process. No extra process, no network ports, no heavy configuration. |

---

## Why Windows?

Most ultra-high-performance telemetry tools (Vector, eBPF-based agents, Fluent Bit) are designed Linux-first. 
On Windows Server, which powers critical IIS infrastructure, high-throughput enterprise .NET apps, and low-latency C++ services, developers are often left with two undesirable options:

1. **Heavy managed logging frameworks** that cause Garbage Collection (GC) pauses and high CPU overhead under heavy traffic spikes.
2. **Cross-platform ports** that wrap Linux paradigms, losing performance through abstraction layers.

**PULP was built to solve this exact problem.**

Rather than using generic cross-platform wrappers, PULP leverages bare-metal Windows primitives:
- Direct **Thread-Local Storage (TLS)** for zero-contention hot path ingestion.
- Native **Windows Thread Pool API** for asynchronous compression tasks.
- Hardware-accelerated **AVX2 / SIMD intrinsics** tuned for x64 architecture.

By committing fully to the Win32 ecosystem, PULP delivers line-rate ingestion with zero GC impact and a deterministic memory footprint under any load.

---

## Features

- **Blazing fast** – zero‑allocation hot path, per‑thread TLS caches, zero-eviction temporal cache, custom SIMD‑accelerated IP masking (AVX‑2).
- **On‑the‑fly compression** – lossless semantic precompression paired with LZ4 reduces I/O volume before writing to disk.
- **Deterministic memory footprint** – no memory spikes during activity surges; the architecture absorbs load seamlessly.
- **Dual‑mode IP anonymisation** – configurable masking for IPv4 and IPv6, including support for CIDR, zones, and compressed addresses.
- **Inter-thread atomic numeration** – easily reconstruct the absolute sequence order of millions of events/logs.
- **Automatic file rotation** – smooth handle swapping and asynchronous deferred close eliminate write stalls.
- **Resilient disk handling** – automatic fallback to a backup path on disk full, access denied, or path not found.
- **Resilient file format** – Each log file is a sequence of independent blocks (batches), making the storage resilient to corruption.
- **Rich telemetry** – JSON statistics endpoint (cache hit ratios, compression ratios, throughput, backpressure, and more).
- **Dual licensing** – AGPLv3 for open‑source use, or a commercial license for closed‑source products.

---

## Performance

Measured on a Lenovo ThinkPad P14s Gen 5 (Ryzen 5 Pro 8640HS, 96 GB RAM, NVMe SSD, Windows 11).

Workload: 1 000 unique URLs × 5 000 unique IPs, random distribution.

**High-Performance Mode (6 caller threads):**
- 250,000,000 logs in 11.52 seconds
- 21.71M logs/second sustained
- ~105MB RAM footprint
- LZ4-only ratio: 1.5× (67% of original)
- End-to-end ratio: ~5.16× (preprocessing + LZ4: 19.4% of the original)
- 0 logs lost, 0 backpressure events

**Economy Mode (single caller thread):**
- 250,000,000 logs in 29.52 seconds
- 8.47M logs/second
- ~16MB RAM footprint
- LZ4-only ratio: 1.5× (67% of original)
- End-to-end ratio: ~4.66× (preprocessing + LZ4: 21.5% of the original)
- 0 logs lost, 0 backpressure events

Performance depends on the hardware, the number of threads, the data entropy, cardinality, and the parameters passed to the initialisation function.

---

## Requirements

- Windows 10 / 11 or Windows Server 2016+ (x64).
- CPU with **AVX2** support (all modern x86‑64 processors).  
- Visual Studio 2022 (solution provided).

---

## Quick Start

A complete working example is provided in the `LogProducer` project.
All values are 'per-thread'. All the caller threads use the same init values.
The DLL is intended to be called by a pool of (or one unique) long lived thread.

```c
#include "pulp.h"

int main() {
    // One‑time initialisation
    uint8_t rc = PulpInit(
        "C:\\Logs",                // primary log folder
        "D:\\BackupLogs",          // backup folder (optional, can be "")
        "C:\\Errors",              // error log folder
        1,                         // enable/disable atomic inter‑thread sequence IDs
        ANON_IP_2,                 // enable/disable anonymization on last 4 octets (IPv4) / 4 hextets (IPv6)
        1,                         // enable/disable strip URL query parameters (if URLs are to be logged)
        128,                       // file rotation every 128 batches (must be power of 2)
        COMPRESSION_BALANCED,      // LZ4 compression level
        BATCH_8MB,                 // 8MB active buffer or 262 144 in-flight logs
        DICT_256K                  // 256k slots cache
    );

    if (rc != RTN_OK) {
        // Handle initialisation error
        return 1;
    }

    // Write a log/evt entry from any thread
    uint16_t res = PulpWrite(
        12,                                          // Numeric identifier of the operation (HTTP method, ICMP type, DNS opcode, etc.), user defined
        "https://www.resource/admin/overview.jpeg",  // Generic resource reference (URL, domain name, ICMP message, etc.) 563 char MAX, see API header for truncation politic.
        40,                                          // Length of the resource string without the terminating char.
        200,                                         // Generic response or error code (HTTP status, DNS RCODE, ICMP code/type, etc.)
        "172.21.22.23",                              // Target address (IPv4/IPv6, hostname, DNS server, etc.). Do NOT try IP_ANON on non-IP endpoints.
        12,                                          // Length of the endpoint string
        4520,                                        // Duration in milliseconds (0–65535)
        8,                                           // Data size bucket (0 = 1–5 KB, 1 = 5–10 KB, etc.), user defined
        123,                                         // Free bitmask (bit 0 = encrypted, bit 1 = protocol version, bit 2 = fragmented, etc.), user defined
        1780008801123456                             // High-resolution timestamp in microseconds (UTC ISO-8601) ("2026-07-29T14:53:21.123456Z" as per the example)
    );
    
    uint8_t backpressure = res & 0xFF;
    uint8_t error        = res >> 8;

    // Graceful shutdown
    PulpShutdown();
    return 0;
}
```

---

## How to Test

The `LogProducer` project lets you verify PULP's performance on your own
hardware in minutes.

1. Clone the repository and open `PULP.sln` in Visual Studio 2022.
2. Build the solution in **Release | x64** configuration.
3. Open `LogProducer.vcxproj` in a new Visual Studio instance and build it.
4. Copy `Pulp.dll` and `Pulp.lib` from `PULP/x64/Release/` into `LogProducer/x64/Release/`.
5. Customize the test (paths, number of threads, PULP options) in `LogProducer.cpp` – it's straightforward.
6. Run `LogProducer.exe` from the console.

Test datasets are included. Results (throughput, compression ratio, memory
footprint, lost logs) are printed directly to the console.


---

## API Overview

The public API is declared in **`pulp.h`** and exported by `pulp.dll`.

| Function | Description |
|----------|-------------|
| `uint8_t PulpInit(...)` | One‑time global initialisation. Configures paths, IP anonymisation, compression level, buffer/cache sizes and internal thread pools. Must be called before any other API function. |
| `uint16_t PulpWrite(...)` | Hot‑path logging function. Accepts a URL, HTTP status, IP, timing, etc. Returns a packed status: high byte = system error, low byte = backpressure level. |
| `char* PulpGetStats()` | Returns a JSON string containing live telemetry (throughput, cache hit ratios, compression ratio, queue depth, errors). The caller must free the string with `Pulp_FreeStats()`. |
| `void PulpFreeStats(char* p)` | Frees a statistics string previously obtained from `Pulp_GetStats()`. |
| `void PulpShutdown()` | Graceful shutdown: flushes all pending data, waits for compression and writes to complete, releases all resources. |

For detailed parameter descriptions, see the API header `pulp.h`.

---

## Configuration Reference

### Cache size (`DictSize`)

> **How the dictionary achieves high compression with few slots**: Even with only 16K slots, PULP can handle millions of unique URLs/IPs over time. 
The dictionary persists across batches; once an entry is cached, all future occurrences reference the same index. 
You only need enough slots to hold your *current hot* working set (frequently repeated values), not every unique string ever seen.
Undersized dictionaries perform very well, while oversized dictionaries can be slightly detrimental.
The L1/L2/L3 hash cascade ensures fast lookup even under cache pressure.
See the API header pulp.h for more details.

Number of unique URL/IP values the per‑thread cache can hold.

| Value | Slots | Recommended batch |
|-------|-------|-------------------|
| `DICT_16K` | 16 384 | `BATCH_500KB` |
| `DICT_32K` | 32 768 | `BATCH_1MB` or lower|
| `DICT_64K` | 65 536 | `BATCH_2MB` or lower|
| `DICT_128K` | 131 072 | `BATCH_4MB` or lower|
| `DICT_256K` | 262 144 | `BATCH_8MB` or lower|
| `DICT_512K` | 524 288 | `BATCH_16MB` or lower|
| `DICT_1M` | 1 048 576 | `BATCH_32MB` or lower|
|`DICT_2M`	|2 097 152|	`BATCH_64MB` or lower|
|`DICT_4M`	|4 194 304|	`BATCH_128MB` or lower|
|`DICT_8M`	|8 388 608|	`BATCH_256MB` or lower|
|`DICT_16M`	|16 777 216| `BATCH_512MB` or lower|
| `DICT_AUTOSIZE` | ~1/32 total RAM | auto |

### Batch size (`BatchSize`)

Controls the flush threshold and the maximum in-flight logs (lost on a hard crash, power outage, etc. The write queue can also hold batches waiting to be processed).

| Value | Size | Max loss per thread | Use case |
|---|---|---|---|
| `BATCH_500KB` | 500 KB | ~15 600 logs | Generally adequate, higher I/O usage, lower compression (less context) |
| `BATCH_1MB` | 1 MB | ~32 768 logs | 
| `BATCH_2MB` | 2 MB | ~65 536 logs | 
| `BATCH_4MB` | 4 MB | ~131 072 logs |
| `BATCH_8MB` | 8 MB | ~262 144 logs |
| `BATCH_16MB` | 16 MB | ~524 288 logs |
| `BATCH_32MB` | 32 MB | ~1 048 576 logs |
| `BATCH_64MB` | 64 MB | ~2 097 152 logs |
| `BATCH_128MB` | 128 MB | ~4 194 304 logs |
| `BATCH_256MB` | 256 MB | ~8 388 608 logs | 
| `BATCH_512MB` | 512 MB | ~16 777 216 logs | Generally higher compression, high memory usage, low I/O usage with write spikes |
| `BATCH_AUTOSIZE` | auto | auto | Auto-selected by hardware |

Flush is triggered by buffer pressure only (no timer). Call `Pulp_Shutdown()` to flush remaining logs on shutdown.

### Compression level (`Lz4CompressionLevel`)

Since compression isn't the bottleneck, `COMPRESSION_BALANCED` is generally recommended.

| Value | Description |
|-------|-------------|
| `COMPRESSION_FAST` | LZ4 fastest mode |
| `COMPRESSION_BALANCED` | LZ4 default – best ratio/speed tradeoff (recommended for most workloads) |
| `COMPRESSION_NONE` | Pre‑compression only, no LZ4 – useful for debugging, more I/O pressure |

### IP anonymisation (`Anon_lvl`)

| Value | IPv4 result | IPv6 result |
|-------|-------------|-------------|
| `ANON_IP_NONE` | `192.168.2.23` | full address |
| `ANON_IP_1` | `192.168.2.x` | last 2 hextets masked |
| `ANON_IP_2` | `192.168.x.x` | last 4 hextets masked |
| `ANON_IP_3` | `192.x.x.x` | last 6 hextets masked |
| `ANON_IP_4` | `x.x.x.x` | full address masked |

> ⚠ Only use IP anonymisation on actual IP addresses. Do not apply it to hostnames or other non‑IP strings.

---

## Integration Suggestions

PULP is designed to be **glued** into your existing infrastructure. As a C native Library it can be interfaced with virtually everything. It writes compressed `.bin` files locally; you decide how to move and process them.

Hereafter are some implementations ideas, which would require adding only basic extensions to the project (2 & 3) or are ready to use out of the box (1 & 4).

### 1. Optimally compressed local archiving for high traffic appliances

Just link PULP, call `PulpWrite()`, and the compressed shards accumulate in your log folder. Use `PulpReader` later to decode them.

### 2. Centralised logging with a network share

All servers write to `\\nas\logs\`. A scheduled task on the central machine runs `PulpReader` against new files and pipes the output to your observability platform.

```powershell
# Example PowerShell script, with json format and output filter extensions (not included in the sources, but easily implementable)
Get-ChildItem \\nas\logs\ -Filter *.bin | ForEach-Object {
    PulpReader.exe --input $_.FullName --format json --filter "http_code>=400" |
        Invoke-RestMethod -Uri "[https://api.datadog.com/v1/input](https://api.datadog.com/v1/input)" -Method Post
}
```

### 3. Direct export via stdout (currently the PulpReader only output integral text)

```cmd
PulpReader.exe --input C:\Logs\shard_12345.bin --format json --filter "http_code>=500" |
    curl -X POST [https://api.datadog.com/v1/input](https://api.datadog.com/v1/input) -H "Content-Type: application/json" -d @-
```

### 4. SIEM / cold storage decoding

```bash
PulpReader.exe E:\Archives\2025-01-01.bin E:\Archives\2025-01-01.txt
```

---

## Economic Impact & Cost Savings

Logging infrastructure and cloud providers charge heavily for **network egress**
and **storage capacity**. By compressing logs directly at the source, PULP reduces
the data footprint by **2× to 5×** before it leaves your application process,
depending on log structure and entropy.

*The figures below are illustrative, based on a conservative 3× compression
factor. You can measure savings on your own workloads using the included
`LogProducer` benchmark project.*

| Raw Daily Log Volume | Typical Use Case | With PULP (3× footprint) | Estimated Annual Savings* |
| :--- | :--- | :--- | :--- |
| **50 GB / day** | SaaS Startup / SME | ~17 GB stored/shipped | **~$2,500 – $3,500** |
| **500 GB / day** | Mid‑Market / Growing Tech | ~167 GB stored/shipped | **~$25,000 – $35,000** |
| **5 TB / day** | Enterprise / High Traffic | ~1.7 TB stored/shipped | **~$250,000+** |

*\*Savings reflect bandwidth, local/cold storage, and self-hosted cluster capacity (Elastic, Loki).*  
*A lifetime commercial license typically pays for itself within weeks or days on high-volume nodes.*

---

## Architecture Overview

```
Pulp_Write()  →  TLS context  →  URL/IP cache  →  active buffer
                    ↑                                ↓ (buffer full)
                    |                             PulpFlush()
                    |                                ↓
                    |                             BuildDictionaryInMemory()
                    |                                ↓
                    |                            CompressionTask (thread pool)
                    |                                ↓
                    +——— WritePool_Enqueue() → WriteThread → WriteFile()
                                                             ↓
                                                       RotationThread (file creation, handle swap)
```

Every thread owns its own cache and active buffer – no lock contention on the hot path. The write pool is a multi‑producer / multi‑consumer queue synchronised with slim reader‑writer locks and condition variables.

For a deep dive into the design choices and performance measurements that drive PULP's architecture, see this companion technical article:
[Processing 250M logs in 11.5s on a laptop with on-the-fly 5× compression](https://medium.com/@fgauthier_36718/loggr-processing-250m-logs-in-11-5s-on-a-laptop-with-on-the-fly-5-compression-7903b3f941d4)

---

## File Format & Resilience

PULP uses a **block-based binary format** engineered for high-throughput streaming writes and total thread-level isolation. A `.bin` archive is a continuous sequence of independent, self-contained compressed blocks (batches).

```
+-----------------------------------------------------------------------+
|                              LOG FILE                                 |
| +-------------------+ +-------------------+     +-------------------+ |
| |  Block 0 (Batch)  | |  Block 1 (Batch)  | ... |  Block N (Batch)  | |
| +-------------------+ +-------------------+     +-------------------+ |
+-----------------------------------------------------------------------+
```

### 1. On-Disk Block Structure

Each block on disk consists of a lightweight header followed immediately by the payload:

| Field | Type / Size | Description |
| :--- | :--- | :--- |
| **`compSize`** | `uint32_t` (4 bytes) | Byte length of the compressed payload on disk |
| **`decompSize`** | `uint32_t` (4 bytes) | Expected byte length of the decompressed payload |
| **Payload** | `compSize` bytes | Raw LZ4 compressed block (or uncompressed if `compSize == decompSize`) |

### 2. Decompressed Payload Layout

Once decompressed, the payload contains two primary sections delimited by 64-bit canary markers (`DICT_BEGIN` and `DICT_END`):

---

## Live Telemetry

`PulpGetStats()` returns a JSON snapshot (caller must free with `PulpFreeStats()`):

```json
{
 cache_hit resource L1 : 249937807
 cache_hit resource L1 % : 99.98
 cache_hit resource L2 : 62193
 cache_hit resource L2 % : 0.02
 cache_hit resource L3 : 0
 cache_hit resource L3 % : 0.00
 cache_hit endpoint L1 : 250000000
 cache_hit endpoint L1 % : 100.00
 cache_hit endpoint L2 : 0
 cache_hit endpoint L2 % : 0.00
 cache_hit endpoint L3 : 0
 cache_hit endpoint L3 % : 0.00
 resource_cache_probes_total : 206998729
 endpoint_cache_probes_total : 4944735
 resource cache_insert_to_step_ratio : 1.21
 endpoint cache_insert_to_step_ratio : 50.56
 resource cache_probes_depth_max : 32
 endpoint cache_probes_depth_max : 0
 resource_cache_fullprobescan_total : 0
 endpoint_cache_fullprobescan_total : 0
 log_processed_total : 250000000
 batch_flushed_total : 15
 batch_compressed_total : 15
 batch_written_total : 15
 writer_waitfile_max : 0
 backpressure_count : 0
 compression_lz4_ratio_avg : 0.71
 compression_failure_total : 0
 lost_logs_total : 0
 log_rotation : 1
 log_refused_total : 0
 rotation_resync_total : 0
 throughput: 26.49 millions logs/s
}
```

> `PulpGetStats()` introduces a ~2 s measurement window. Call it at most every few seconds with a dedicated thread.

---

## Limitations

- **Windows x64 only.** The code uses Windows‑specific APIs (TLS, SRW locks, thread pools). A Linux port is planned.
- **No transactional durability for in‑flight data.** Logs in the active buffer are lost on a hard crash. Loss is bounded to `BATCH_SIZE / 32` entries per thread + the write queue if any (it's usually empty due to the speed of the write pool). Batches already flushed to disk are safe.
- **Flush triggered by throughput only** – no background timer. Low‑traffic applications should use the smallest batch size (15 600 logs) probably with a unique long lived thread.
- **Fixed log schema.** The PulpWrite() signature accepts a fixed set of fields (operation, resource, endpoint, status, timing, flags). 
  These cover HTTP, DNS, ICMP and similar tabular log formats. Fully arbitrary structured fields would require extending the source.

---

## Contributions & project philosophy

Contributors must adhere to the Contributor Licence Agreement ([CLA.md](CLA.md)) located at the root of the project. 
Take time to read it before contributing.
The document also states the project's philosophy regarding the dual-license.

---

## Author

PULP was created by **François Gauthier** – Founder & Software Architect, [Superwired-labs](https://www.linkedin.com/in/superwired-labs/).

---

## License

PULP is dual‑licensed:

- **Open Source** – GNU Affero General Public License v3.0 (AGPLv3)
- **Commercial** – a proprietary license for closed‑source products

Full license texts are in the `LICENSES/` folder.  
For commercial conditions & pricing, contact **fgauthier [at] superwired-labs [dot] com**.

---

## Community Sponsorship & Enterprise Support

PULP is open-source software maintained by **SuperWireLabs**. If PULP saves you storage costs or accelerates your infrastructure, consider supporting its ongoing development:

* **Open Source Sponsorship:** Support the project via [GitHub Sponsors](https://github.com/sponsors/superwired-labs) to help fund roadmap features and maintenance.
* **Commercial Licensing & Consulting:** For custom binary layouts, dedicated support, or integration services into enterprise architectures, contact us at `fgauthier[at]superwired-labs[dot]com` or read our [Commercial Licensing Guide](COMMERCIAL.md).

---

### Third‑Party Libraries

| Library | Author | License |
|---------|--------|---------|
| [CityHash](https://github.com/google/cityhash) | Google Inc. | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | Yann Collet | BSD‑2‑Clause |
| [LZ4](https://github.com/lz4/lz4) | Yann Collet | BSD‑2‑Clause |

See `THIRD_PARTY.md` for details.
