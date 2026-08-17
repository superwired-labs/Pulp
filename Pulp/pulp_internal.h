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

#include "pulp.h"
#include "lz4.h"
#include "stats.h"
#include <math.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <Windows.h>

// cache configuration
#define PROBE_RANGE_PRIMARY         32
#define PROBE_RANGE_SECONDARY       64
#define PROBE_RANGE_TERTIARY        256
#define HASH_SEED                   0xCAFEBABEBADBEEF


// General configuration
#define DICT_BEGIN                  0xDEADBEEFCAFEBABE                // PULP and Reader Version control
#define DICT_END                    0xBEEFBABEDEADCAFE
#define MAX_METHOD_LEN              30
#define MAX_URL_LEN                 563
#define MAX_IPV6_LEN                79
#define URL_PREFIX_LEN              80                               
#define URL_SUFFIX_LEN              MAX_URL_LEN - URL_PREFIX_LEN - 4  // - 1 byte for the terminator and -3 for the separator '...'
#define COMPRESSION_LEVEL_DEFAULT   COMPRESSION_BALANCED              // LZ4 default
#define IP_ANON_DEFAULT             ANON_IP_NONE                      // No anon by default
#define TIMEOUT_MS                  500
#define SPIN_LIMIT                  20
#define FLUSH_PER_FILE_DEFAULT      512                                // Have to be power of 2. file size = FLUSH_PER_FILE_DEFAULT * BUFFER SIZE (default is 512 * 64 MB)
#define LO_PRESSURE                 0.6
#define MED_PRESSURE                0.75
#define HI_PRESSURE                 0.9
#define MIN_COMPRESSORS             2


// DEBUG
#define DEBUG_PULP_ACTIVE 0
// TRACE
#define TRACE_PULP_ACTIVE 0


#if DEBUG_PULP_ACTIVE
#define LOG_DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n",  ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...)
#define LOG_ERROR(fmt, ...)
#endif
#if TRACE_PULP_ACTIVE
#define LOG_TRACE(fmt, ...) printf("[TRACE] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...)
#endif

// Write task type
typedef enum {
	TASK_ERROR,                    // Task for errors logging 
	TASK_WRITE,                    // Task for writing a file block
	TASK_CREATE_OR_ROTATE          // Task for signaling the writing of the last block of a file before closing the handle
} WriteTaskType;

// Block header for decompression by the reader
typedef struct {
	uint32_t compSize;
	uint32_t decompSize;
} BlockHeader;

typedef struct {
	uint64_t hash;                              // Precomputed hash
	uint8_t code;                               // Numeric code
	uint8_t length;                             // Verb length
} HttpVerbEntry;

typedef struct __declspec(align(32)) {
	uint64_t sequence;
	uint64_t timestamp;
	uint32_t url_idx;
	uint32_t ip_idx;
	uint16_t http_code;
	uint16_t duration_ms;   // 0-65s
	uint16_t response_size; // 0-65KB
	uint8_t  http_verb;
	uint8_t  flags;         // Bits for SSL, cache, etc.
} SerializedEntry;

typedef struct __declspec(align(64)) {
	uint64_t hash;
	uint16_t value_len;
	uint8_t used_in_shard;
	char padding[1];                            // alignment on 8 bytes for value
	char value[MAX_URL_LEN + 1];                // +1 for '\0', total 320 bytes (5 cache lines)
} CacheEntryURL;

typedef struct __declspec(align(32)) {
	uint64_t hash;
	uint8_t value_len;
	uint8_t used_in_shard;
	char padding[6];                            // Align to 16 bytes
	char value[MAX_IPV6_LEN + 1];               // +1 for '\0' total 96 bytes
} CacheEntryIPV6;


typedef struct __declspec(align(64)) {
	SerializedEntry* active_buffer;
	BYTE* pending_buffer;
	CacheEntryURL* url_cache;
	CacheEntryIPV6* ipv6_cache;
	size_t active_count;
	size_t buffer_capacity;
	size_t pending_size;
	HANDLE file_handle;
	SRWLOCK  file_lock;                                             // Protects file_handle for WriteFile & CloseHandle
	__declspec(align(64)) long pending_write;
	__declspec(align(64)) CONDITION_VARIABLE pw_complete;           // Wake-up on pending_write completion 
	__declspec(align(64)) LONG pending_compression;
	__declspec(align(64)) CONDITION_VARIABLE pc_complete;           // Wake-up on pending_compression completion
	CRITICAL_SECTION compression_cs;          // Associated lock
	CONDITION_VARIABLE file_ready_cv;         // Shard creation ready
	uint32_t flush_count;
	char ipdest[96];                          // Fixed buffer for anonymized IPs
	uint32_t* nb_urls_dicts;                  // URL dictionaries for preallocation
	size_t nb_urls_dicts_size;                // The URL dictionaries preallocated size
	uint32_t* nb_ips_dicts;                   // IP dictionaries for preallocation
	size_t nb_ips_dicts_size;                 // The IP dictionaries preallocated size
	BYTE* dictbuf;                            // Complete dictionary buffer
	size_t dictbuf_size;                      // The dictionary buffer size
	BYTE* databuf;                            // Data buffer (without dictionary)
	size_t databuf_size;                      // The data buffer size
	size_t pending_buffer_size;               // The size allocated to pending_buffer (do not confuse with pending_size)
} ThreadContext;

typedef struct __declspec(align(64)) {
	ThreadContext* ctx;						  // 8 bytes
	BYTE* data;								  // 8 bytes
	size_t size;							  // 8 bytes
	HANDLE file_handle;						  // 8 bytes
	uint32_t taskType;						  // 4 bytes (or 32-bit enum)
	uint32_t reserved;						  // 4 bytes to align to 64
	uint8_t padding[24];					  // complete to 64 bytes
} WriteTask;


typedef struct {
	WriteTask* tasks;                        // Circular file
	CRITICAL_SECTION queuelock;              // Protects concurrent access to queues
	CRITICAL_SECTION filelock;               // Protects concurrent access to files (writes)
	CRITICAL_SECTION rotationlock;           // Protects concurrent access to files (rotations)
	CRITICAL_SECTION errorlock;              // Protects concurrent access to files (errors)
	CRITICAL_SECTION pathswap;               // Protects concurrent access to paths
	HANDLE* threads;                         // Writer threads
	HANDLE writetask_semaphore;              // Synchronization
	__declspec(align(64)) LONG running;      // Pool state
	__declspec(align(64)) LONG head;
	__declspec(align(64)) LONG tail;
	uint32_t queue_size;                     // Actual size
	uint32_t threads_count;                  // Number of threads
} WritePool;

typedef struct ContextList {
	ThreadContext* ctx;
	struct ContextList* next;
} ContextList;

//
// Context structure for passing parameters into the InitOnce callback
//
typedef struct _LogInitParams {
	const char* log_path;                  // Base directory for log shards
	const char* backup_path;               // Secondary directory in case of failure
	const char* error_path;			       // Error logs
	uint8_t enableSeqFlag;                 // 0 = disable sequence numbers, non-zero = enable
	Anon_lvl ip_anon_lvl;                  // lvl of IP information
	uint16_t flush_per_file;               // number of flushes (buffer size) per file
	Lz4CompressionLevel clvl;              // compression level
	BatchSize bufferSizeLevel;             // Which buffer size to use
	DictSize cache_size;                   // Cache size
	uint8_t truncate_url_params;           // Enable params truncation
} _LogInitParams;


typedef BOOL(WINAPI* PFN_GetPhysInstalled)(
	PULONGLONG TotalMemoryInKilobytes
	);


// global variables
extern __declspec(align(64)) LONG64 global_sequence;
extern __declspec(align(64)) LONG64 global_file_counter;
extern char base_path[MAX_PATH];
extern char backup_path[MAX_PATH];
extern volatile LONG use_backup_path;
extern char error_path[MAX_PATH];
extern HANDLE herror;
extern DWORD tls_index;
extern volatile BOOL enable_sequence;
extern WritePool global_write_pool;
extern uint8_t global_compression_level;
extern Anon_lvl ip_anon_lvl;
extern uint16_t flush_per_file;
extern uint32_t g_buffer_size;
extern uint64_t g_cache_size;
extern ContextList* global_context_list;
extern CRITICAL_SECTION context_list_lock;
// Environment variable
extern uint32_t g_write_pool_thread_count;
extern uint32_t g_max_pending_handles;
extern uint32_t g_max_rotation_queue;
extern uint32_t g_write_queue_size;
// Initialization for TPT
extern TP_CALLBACK_ENVIRON g_envCompress;
extern PTP_POOL g_threadPool;

// Main compression callback
void CALLBACK CompressionTask(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK);


extern char* GetShardPath(LONG64 file_num);
extern HANDLE CreateErrorFile(char* errorpath);
extern void WriteError(HANDLE herror, char* msg);
extern void WritePool_Shutdown();
extern void WritePool_Init();
extern void InitGlobalConfig();
extern void WritePool_Enqueue(WriteTask task);
extern uint8_t PulpFlush(void* ctx_override);

