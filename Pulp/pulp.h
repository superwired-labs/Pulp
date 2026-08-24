/*
 * PULP (Precompressed Upstream Layer Pipeline)
 * High‑performance, low‑latency telemetry and logging layer for Windows.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Superwired-Commercial-v1.0
 *
 * Copyright (C) 2026  Francois Gauthier - Superwired-Labs
 *
 * ===================== DUAL LICENSE NOTICE =====================
 * This program is available under a dual License model:
 * The GNU Affero General Public License v3.0 (AGPLv3)
 * or a commercial license from Superwired-Labs.
 *
 * ===================== FREE LICENSE ============================
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ===================== COMMERCIAL LICENSE ======================
 * This software is available under a commercial license from Superwired-Labs,
 * if you intend to use this software in a closed-source product or service without
 * complying with AGPLv3 copyleft terms, a commercial license is required.
 *
 * Full license texts are located in the LICENSES/ directory at the root of this
 * repository. See COMMERCIAL.md for licensing options and contact information.
 */

#pragma once

#ifdef PULP_EXPORTS
#define DLL_API __declspec(dllexport)
#else
#define DLL_API __declspec(dllimport)
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------- STRUCTURES DEFINITION ---------------------------------------------*/

/* Configurable dictionary size :
* 
* => The dictionary size is the number of unique elements the cache can hold. You can select a dictionary size approximatly related to 
*    the expected working set uniqueness number, or related to the amount of RAM you want to be used by the dictionary. 
*    The cache algorithm is efficient even for severely under-sized dictionary. If it fits into the CPU cache, it will perform very well.
*    For instance, a 512K elements dictionary can handle millions of unique values efficiently provided it is paired with an 
*    adequate batch size (more on this in the following section).
*    
* => While under-sized dictionary performs well, an (exageratly) oversized dictionary can be detrimental.
* 
* => If you want the smaller memory footprint, choose a 16K slots dictionary size and pair it with a batch size using this formula :
*    DICT_SIZE * 32768 = BATCH_SIZE in byte. (in that case => 16 * 1024 * 32 = 524 288 bytes (or 500KB);
*    
* => The PULP use a FIXED dictionary size under any load, thanks to an optimized data management algorithm.
*    This makes the resource consumption very predictable and help to achieve high performances.
* 
* => If you don't know what data uniqueness to expect, select the DICT_AUTOSIZE option.
*    The dictionary size (and the batch size) will then be adjusted following the hardware specs.
*    The whole PULP should take about 1/16 of the total RAM when the DICT_AUTOSIZE option is selected.
* 
* => To estimate the dictionary memory footprint use :
*    dictionary size : count_of_dictionary_slots * 563 (slot size) * number_of_caller_threads = total_size_in_bytes
*    For instance : DICT_64K is 64 * 1024 => 65536 elements, so 65536 * 563 * 1 (number of caller thread) = 36.9 MB
* 
* 
* ── CHEAT SHEET ──────────────────────────────────────────────────────────
*  Preset       Slots      Approx. RAM (per thread)   Recommended Batch
*  DICT_16K      16 384				  ~9 MB						500 KB 
*  DICT_32K      32 768				 ~18 MB						  1 MB 
*  DICT_64K      65 536				 ~37 MB						  2 MB 
*  DICT_128K    131 072				 ~74 MB						  4 MB 
*  DICT_256K    262 144				~148 MB						  8 MB 
*  DICT_512K    524 288				~295 MB						 16 MB 
*  DICT_1M    1 048 576				~590 MB						 32 MB 
*  DICT_2M    2 097 152				~1.2 GB						 64 MB 
*  DICT_4M    4 194 304				~2.4 GB						128 MB  ---------------->
*  DICT_8M    8 388 608				~4.7 GB						256 MB   EXPERIMENTAL, LZ4 MAX = 2GB or about 1.3 million dictionary entries. But the calculation should account for the batch size also. Use at your own risks or replace the compressor.
*  DICT_16M  16 777 216				~9.4 GB						512 MB  <---------------- 
*
*  Memory formula :  slots × 563 bytes × nb_threads
* 
* 
*/
typedef enum {
	DICT_AUTOSIZE = 0, // Autosize the cache using a machine spec ratio
	DICT_16K = 16,     // approx. 16384*563*1 = 9.2 MB, 16384*563*2 = 18.4 MB, 16384*563*3 = 27.6 MB, ...
	DICT_32K = 32,     // approx. 32768*563*1 = 18.4 MB, 32768*563*2 = 36.8 MB, 32768*563*3 = 55.2 MB, ...
	DICT_64K = 64,     // ...
	DICT_128K = 128,   // ...
	DICT_256K = 256,   // ...
	DICT_512K = 512,   // ...
	DICT_1M = 1024,    // approx. 1048576*563*3 = 590 MB,       ...         , 1048576*563*3 = 1.7 GB, ...
	DICT_2M = 2048,    // ...
	DICT_4M = 4096,    // ...
	DICT_8M = 8192,    // approx.   8388608*563*1 = 4.7 GB,       ...       , 8388608*563*6 = 28.3 GB, ...
	DICT_16M = 16384   // approx.  16777216*563*1 = 9.44 GB,       ...       ,         ...
} DictSize;

/* Configurable Batch size :
* 
* => If you KNOW in advance the number of unique resources (URLS, domains, ...) you will log, you can opt for a Batch size that can 
*    hold more elements than the selected dictionary size. 
*    This will result in a better compression ratio, benefiting storage and egress costs, provided the dictionary size is large 
*    enough to hold all the necessary unique values (hard requirement). 
*    This is not the recommended setting, as the risk to saturate the cache is real (if this happen the PULP will switch to a safer 
*    Batch/dictionary ratio, desaturate the dictionary/cache and keep running). 
*
* => If you don't know which Batch Size to use, you can use BATCH_AUTOSIZE. That will automatically select a value for both the batch and
*   the dictionnary, following the detected hardware specs. The AUTOSIZE selection will work fine against any workload,
*    and provide an adequate compression ratio.
* 
* => If you want to reduce the number of in-flight logs, use a Batch size smaller or equal to the dictionary size :
*    this will result in less latency, less in-flight logs, more I/O, and probably worse compression (data dependant).
*    For instance, if a dictionary size of 512K (elements count) is selected, any batch size from 500KB to 16MB will work fine.
*    To determine the recommended Batch size for any dictionary  size use the following formula :
*    Selected dictionary Size * 1024 * 32 = Best Batch Size (in Bytes).
*    There's no real recommended minimum Batch Size, but tiny batches can be detrimental for performance in a millions logs/s env.
*    Use the following formula to determine the max batch size :
*    DICT_SIZE * 32768 = BATCH_SIZE in byte. 
*    For instance : DICT_128K => 128 * 1024 * 32 = 4 194 304 bytes (so BATCH_4MB);
* 
* => To estimate the number of in-flight logs : BATCH_SIZE / 32 = number_of_inflight_logs (per thread)
*    (millions of logs/s with the highest compression ratio require millions of in-flight logs)
*/
typedef enum {
	BATCH_500KB = 0,     // 500 KB        The smallest batch size
	BATCH_1MB = 1,       // 1 MB ------------|
	BATCH_2MB = 2,       // 2 MB          Low memory usage
	BATCH_4MB = 4,       // 4 MB          (more I/O,less latency)
	BATCH_8MB = 8,       // 8 MB ------------|      
	BATCH_16MB = 16,     // 16 MB          Normal
	BATCH_32MB = 32,     // 32 MB           use 
	BATCH_64MB = 64,     // 64 MB           zone
	BATCH_128MB = 128,   // 128 MB ----------|
	BATCH_256MB = 256,   // 256 MB      Large buffers for super efficient compression, should be used with large dictionary sizes on machine with lot of RAM
	BATCH_512MB = 512,   // 512 MB ----------|  
	BATCH_AUTOSIZE = 99
} BatchSize;

/* LZ4 Configurable compression level */
typedef enum {
	COMPRESSION_FAST,     // LZ4 Fastest
	COMPRESSION_BALANCED, // LZ4 Default (Usually best, recommended)
	COMPRESSION_NONE      // No Lz4 Compression, but precompression still apply (mandatory).
} Lz4CompressionLevel;

/* IP anonynimisation levels :
*  ANON_IP_NONE = no anonymisation (192.168.2.23)
*  ANON_IP_1_BYTE = 192.168.2.x
*  ANON_IP_2_BYTES = 192.168.x.x
*  ANON_IP_3_BYTES = 192.x.x.x
*  ANON_IP_4_BYTES = x.x.x.x
*/
typedef enum {
	ANON_IP_NONE = 0,
	ANON_IP_1,   /* IPv4 : 1 byte   / IPv6 : 2 hextets */
	ANON_IP_2,   /* IPv4 : 2 bytes  / IPv6 : 4 hextets */
	ANON_IP_3,   /* IPv4 : 3 bytes  / IPv6 : 6 hextets */
	ANON_IP_4    /* IPv4 : 4 bytes  / IPv6 : 8 hextets (full) */
} Anon_lvl;

/* Error code table */
typedef enum {
	RTN_OK = 0,                   // -> No error 
	RTN_LIGHT_PRESSURE,           //-------|
	RTN_MEDIUM_PRESSURE,          //   Back pressure signals a DISK struggling to absorb the data    
	RTN_HEAVY_PRESSURE,           //-------|
	RTN_LOG_REFUSED,              //   Can happen briefly if the PULP is misconfigured (ie. A dictionary too small)
	RTN_INTERNAL_FAILURE_0,       //-------|
	RTN_INTERNAL_FAILURE_1,       //       
	RTN_INTERNAL_FAILURE_2,       //
	RTN_INTERNAL_FAILURE_3,       //
	RTN_INTERNAL_FAILURE_4,       //    Internal system failure codes (memalloc failure, ...)
	RTN_INTERNAL_FAILURE_5,       //
	RTN_INTERNAL_FAILURE_6,       //
	RTN_INTERNAL_FAILURE_7,       //
	RTN_INTERNAL_FAILURE_8,       //       
	RTN_INTERNAL_FAILURE_9,       //-------|
	RTN_INIT_FAIL_MISSING_PARAM,  //
	RTN_INIT_WARN_DEFAULT_PARAM   //    A param is missing (FAIL) or unexpected (WARN)
}rtn_values;

/*--------------------------------------------- FUNCTIONS DEFINITION ---------------------------------------------*/

/* PULP API :
*  
*  => PulpInit() : Initialize the PULP's configuration and options. This function is called once, before the logging loop begin. Not thread safe.
*  => PulpWrite() : Thread safe, write the logs or events to disk, must be called for each individual logs.
*                  Thread safety is achieved using per-thread caches and lock-light MPMC queues for inter-thread coordination, 
*                  minimizing contention and memory barriers.
*  => PulpShutdown() : Shutdown the PULP, flush all buffers and free all memory. Must be called if stopping the PULP is required. Not thread safe.
*/

/* Specific param info :
*  LOG_PATH, BACKUP_PATH and ERROR_PATH are max 260 char long (Windows MAX_PATH). 
*  ERROR_PATH contains a unique error log in append mode.
*  FLUSH_PER_FILE determines the rotation schedule. One flush is One Batch (see the Batch enum for information about the batch size). The resulting file size
*     is the batch size multiplied by the flush_per_file value. For instance, a batch size of 500KB and a flush_per_file of 128 will produce a log file of 64MB (uncompresed).
*     Since the semantic precomrpession and the LZ4 compression are applied, the resulting file size will be much smaller than the uncompressed size. The actual file size is data dependent.
*     The default value is 128, which is a good compromise between I/O and file size. The reasonable range probably is 64 to 1024, depending on NVMe SSD speed, activity and the expected log volume.
*     The PULP does not delete or overwrite previous files. Log rotation produces uniquely named files following the naming convention <timestamp>_<index>.bin.
* 
*  Caution :  ip_anon_lvl is meant for IP anonymization. The IP information should be passed via the endpoint parameter of the PulpWrite() function.
*             Do NOT use ip_anon_lvl (other than ANON_IP_NONE) on endpoints that are not IPv4/6 adresses.
*             truncate_url_params is meant to truncate params from URLS. The URL information should be passed via the resource parameter of the
*             PulpWrite() function. Do not use truncate_url_params on non-URL resources.
* 
*  RTN VALUE : 0 == OK
*             17 == WARNING (a param was invalid and was overridden by a default value).
*             16 == MISSING_PARAM (a param was missing or out of scope).
*  => It is recommended to treat both error codes (17 and 16) as a no-go. 
*/
DLL_API uint8_t PulpInit(
	const char* log_path,           // Path to the log folder (must exist).
	const char* backup_path,        // A backup path (optional, but must exist if specified) in case of log_path failure (disk full, write acces...)
	const char* error_path,         // Path to the error folder (must exist). Text based, append only on execution.
	uint8_t enable_seq,             // Enable (1) or disable (0) atomic (inter-thread) log numbering.
	Anon_lvl ip_anon_lvl,           // IP anonymisation level (each level is a byte/hextet of IPv4/6). Use ANON_IP_NONE if non-IP endpoints are recorded.
	uint8_t truncate_url_params,    // Strip params from URLS (1) or keep params (0). Use 0 if non-HTTP logs are logged.
	uint16_t flush_per_file,        // Number of Batch per file before rotation (default 512), must be power of two. 
	Lz4CompressionLevel level,      // LZ4 Compression level (recommended COMPRESSION_BALANCED).
	BatchSize buffer_size,          // Batch size, must use a BatchSize enum value (see the enum for more info).
	DictSize dict_size);            // Dictionary size, must use a DictSize enum value (see the enum for more info).

/* Specific param info :
*  RESOURCE : Truncated if longer than 563 char, using a prefix (first 80 chars) and sufix (last 477 chars) strategy, 3 chars kept as a visual separator.
*  TIMESTAMP : should be a high-res timestamp (ms precision).
* 
*  RTN VALUE : 	 (((uint16_t)rtn_code_hi << 8) | (uint16_t)rtn_code_lo);
*                rtn_code_hi = System errors (error codes 5 to 14), the log operation is aborted and the PULP should be stopped.
*	             rtn_code_lo = Perf Info, active backoff activity (O == OK, 1 == light pressure, 2 == medium pressure, 3 == high pressure, 4 == log refused).
*                              Refused logs can be sent again to the PULP instantly (usually a saturation problem that the PULP will solve in a
*                              few milliseconds).
* ENDPOINT : The value is limited to 79 chars. Must be null-terminated.
* 
* The field semantic is indicative. Note that both "resource" and "endpoint" are cached and used to populate a dictionary.
* It means they are best suited for repetitive patterns (GUID, TS, etc.. should be assigned to other fields if possible).
* The PULP handles raw byte buffers. Encoding interpretation is left to the reader implementation.
* Note : in case of a crash, in-flight logs are lost. The loss is limited by the parametrized "data-window" (batch size).
* 
* Exemple log format: [79461928] [2025-10-06T12:40:55.904092Z] [file:///C:/logs/shard_14804_26.bin] [122.49.139.58] [409] [DELETE] [48067ms] [23914b] [229]
*                     [atomic id] [timestamp] [resource] [ip] [status_code] [operation] [duration] [data_size] [flags]
*/
DLL_API uint16_t PulpWrite(
	uint8_t operation_id,        // Numeric identifier of the operation (HTTP method, ICMP type, DNS opcode, etc.), match the decoding table on the reader side
	const char* resource,        // Generic resource reference (URL, domain name, ICMP message, etc.), cached and truncated if longer than 563 bytes (prefix + suffix + ellipsis).
	uint32_t resource_len,       // Length of the resource string
	uint32_t status_code,        // Generic response or error code (HTTP status, DNS RCODE, ICMP code/type, etc.)
	const char* endpoint,        // Target address (IPv4/IPv6, hostname, DNS server, etc.). Do NOT try IP_ANON on non-IP endpoints, cached and truncated if longer than 79 bytes.
	uint32_t endpoint_len,       // Length of the endpoint string
	uint16_t duration_ms,        // Duration in milliseconds (0–65535), user defined, no semantic meaning in the PULP.
	uint16_t data_size_bucket,   // Data size bucket (0 = 1–5 KB, 1 = 5–10 KB, etc.), user defined, no semantic meaning in the PULP.
	uint8_t  flags,              // Free bitmask (bit 0 = encrypted, bit 1 = protocol version, bit 2 = fragmented, etc.), user defined, no semantic meaning in the PULP.
	uint64_t timestamp           // High-resolution timestamp in microseconds (UTC ISO-8601)
);

/* PulpGetStats() retrieve live STATS from the PULP in JSON format.
*  The returned buffer must be freed after use (at shutdown).
*  In case the caller can't call native memory management function (free()), a PulpFreeStat(char*) function is provided.
*  PulpGetStats() is not thread safe and may cause performance issues if called too often (every few seconds is fine. The functions takes about 2 seconds to sample de data and return).
*  Must be called in a separate dedictated single thread, and not in the hot path (PulpWrite() pool).
*  Since the stat values are read during the PULP execution, values can change during the reading, resulting in small discrepencies
*  (i.e numbers and % might not perfectly match).
*  Information returned (exemple values) :
*   {
*    cache_url_L1 : 499999                 // number of URLS managed at L1
*    cache_url_L1 % : 100.00               // % of the URL total
*    cache_url_L2 : 1                      // number of URLS managed at L2
*    cache_url_L2 % : 0.00                 //  % of the URL total
*    cache_url_L3 : 0					   // number of URLS managed at L3
*    cache_url_L3 % : 0.00                 //  % of the URL total
*    cache_ip_L1 : 500000                  //  number of IPS managed at L1
*    cache_ip_L1 % : 100.00                //  % of the IP total
*    cache_ip_L2 : 0                       //  number of IPS managed at L2
*    cache_ip_L2 % : 0.00                  //  % of the IP total
*    cache_ip_L3 : 0                       //  number of IPS managed at L3
*    cache_ip_L3 % : 0.00                  //  % of the IP total
*    url_cache_probes_total : 234116       // number of probes performed for URLS
*    ip_cache_probes_total : 55860         // number of probes performed for IPS
*    url_insert_to_step_ratio : 2.14       // number of insert / number of probes (higher is better) for URLS
*    ip_insert_to_step_ratio : 8.95        // number of insert / number of probes (higher is better) for IPS
*    url_probes_depth_max : 32             // max probe depth recorded for URLS
*    ip_probes_depth_max : 0               // max probe depth recorded for URLS
*    url_fullprobescan_total : 0           // number of full-scan for URLS (can happen, not often. Check if throughput is still acceptable)
*    ip_fullprobescan_total : 0            // number of full-scan for IPS (can happen, not often. Check if throughput is still acceptable)
*    log_processed_total : 500000          // total logs processed
*    batch_flushed_total : 32              // total batch processed
*    batch_compressed_total : 32           // total compression processed  
*    batch_written_total : 32              // total batch written
*    writer_waitfile_max : 0               // writer queue depth (lower is better)
*    backpressure_count : 0                // number of backpressure activated
*    compression_lz4_ratio_avg : 0.86      // LZ4 compression average on preprocessed data (the final compression ratio on disk is the result of the precompression ratio and the LZ4 ratio)
*    compression_failure_total : 0         // number of LZ4 failures 
*    lost_logs_total : 0                   // abandonned batches (check error file if that happen)
*    log_rotation : 2                      // file rotation counter
*    log_refused_total : 0                 // number of refused logs on PulpWrite call
*    rotation_resync_total : 0             // number of resync on file creation (should happen rarely, if ever)
*    throughput_million_logs/s: 6.55       // number of million of logs processed per second
*   }
*/
DLL_API char* PulpGetStats();

/* Use only to free the buffer returned by PulpGetStats */
DLL_API void PulpFreeStats(char* stats_ptr);

/* No params :
*  PulpShutdown() must be called whenever the PULP is stopped.
*  This function drain the buffers and queues to disk before freeing memory.
*  Not thread safe, call it ONCE.
* 
*  It displays a collection of STATS upon termination 
*/
DLL_API void PulpShutdown();

#ifdef __cplusplus
}
#endif