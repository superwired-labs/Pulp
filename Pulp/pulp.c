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

 * Full license texts are located in the LICENSES/ directory at the root of this
 * repository. See COMMERCIAL.md for licensing options and contact information.
 */

#define PULP_EXPORTS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "pulp.h"
#include "pulp_internal.h"
#include "lz4.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "cityhash_c.h"
#include <ShlObj.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <synchapi.h>
#include <immintrin.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>



// Global variables
#if PULP_STAT_ACTIVE
_stats g_stats = { 0 };
#endif
CRITICAL_SECTION context_list_lock;
__declspec(align(64)) LONG64 global_sequence;
__declspec(align(64)) LONG64 global_file_counter;

volatile LONG use_backup_path;
char base_path[MAX_PATH] = { 0 };
char backup_path[MAX_PATH] = { 0 };
char error_path[MAX_PATH] = { 0 };
DWORD tls_index = TLS_OUT_OF_INDEXES;
ContextList* global_context_list = NULL;
volatile LONG file_counter = 0;
Anon_lvl ip_anon_lvl = ANON_IP_NONE;
volatile BOOL enable_sequence = FALSE;
uint32_t g_log2_64 = 0;
uint16_t flush_per_file = FLUSH_PER_FILE_DEFAULT;
uint8_t truncate_url_params = 1;
uint8_t global_compression_level = COMPRESSION_LEVEL_DEFAULT;



// One‐time initialization guard
static INIT_ONCE g_loggerInitOnce = INIT_ONCE_STATIC_INIT;
// Compression callback
static TP_CALLBACK_ENVIRON g_envCompress;
static PTP_POOL g_threadPool = NULL;
// Environement variable
uint32_t g_write_pool_thread_count = 0;
uint32_t g_max_pending_handles = 0;
uint32_t g_max_rotation_queue = 0;
uint32_t g_write_queue_size = 0;
uint32_t g_buffer_size = BATCH_16MB * 1024 * 1024;        // buffer size in bytes
uint64_t g_cache_size = DICT_1M * 1024;                   // cache size in slot number
HANDLE herror = INVALID_HANDLE_VALUE;

void InitGlobalConfig() {
	g_write_pool_thread_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	if (g_write_pool_thread_count == 0)
		g_write_pool_thread_count = 6;
	g_max_pending_handles = g_write_pool_thread_count * 12;
	g_max_rotation_queue = g_write_pool_thread_count * 24;
	// Powers of 2
	g_write_queue_size = 1;
	while (g_write_queue_size < (g_write_pool_thread_count * 2048)) {
		g_write_queue_size <<= 1;
	}
}

/*==============================================================================
 * Log2_64 - Calculates floor(log2(x)) / MSB index for a 64-bit integer.
 *
 * Optimized via MSVC _BitScanReverse intrinsics (BSR assembly instruction).
 * Includes a fallback for 32-bit targets (splits into two 32-bit words).
 *
 * Parameter:
 * x - Unsigned 64-bit integer (WARNING: x must be > 0, undefined behavior if x == 0)
 *
 * Return:
 * The 0-based index (0..63) of the Most Significant Bit (MSB) set to 1.
 *============================================================================*/
static inline int Log2_64(uint64_t x) {
	unsigned long index;
#if defined(_M_X64) || defined(_WIN64)
	// 64-bit version
	_BitScanReverse64(&index, x);
#else
	// 32-bit fallback (splits high and low parts)
	if (_BitScanReverse(&index, (unsigned long)(x >> 32))) {
		index += 32;
	}
	else {
		_BitScanReverse(&index, (unsigned long)x);
	}
#endif
	return (int)index;
}


static inline const char* StripBrackets(const char* s, size_t* len)
{
	if (*len >= 2 && s[0] == '[' && s[*len - 1] == ']') {
		*len -= 2;
		return s + 1;               /* pass the '[' character */
	}
	return s;
}

static inline unsigned Ms_ctz(uint32_t v)          // count‑trailing‑zeros
{
#if defined(_M_X64) || defined(_M_IX86)
	unsigned idx;
	// _tzcnt_u32 is faster than _BitScanForward when available
	// (it is part of BMI1/BMI2, present on all AVX2 CPUs).
	idx = (unsigned)_tzcnt_u32(v);
	return idx;                     // if v==0, _tzcnt_u32 returns 32 → ok here
#else
	unsigned long idx;
	_BitScanForward(&idx, v);
	return (unsigned)idx;
#endif
}

static inline unsigned Ms_clz(uint32_t v)          // count‑leading‑zeros
{
	unsigned long idx;
	_BitScanReverse(&idx, v);       // idx = position of the highest bit (0‑based)
	return 31u - (unsigned)idx;     // 31‑idx = number of leading zeros
}


/* ---------------------------------------------------------------------------
 *  IP / Protocol Classifier (Ultra-Fast Scalar Single-Pass)
 *
 *  Returns:
 *   0 = IPv4 (or IPv4 + port)
 *   1 = IPv6 (or IPv6 + port / compressed)
 *  -1 = Invalid / non-IP / Domain name
 * --------------------------------------------------------------------------- */
static inline int ClassifyIpFast(const char* s, size_t n)
{
	s = StripBrackets(s, &n);
	if (n == 0 || n > 80)
		return -1;

	int colon_cnt = 0;
	int dot_cnt = 0;
	int last_colon = -1;
	int last_dot = -1;
	int has_double_colon = 0;
	int invalid_char = 0;
	int prev_was_colon = 0;

	for (size_t i = 0; i < n; ++i) {
		unsigned char c = s[i];

		int is_colon = (c == ':');
		colon_cnt += is_colon;
		last_colon = is_colon ? (int)i : last_colon;

		/* Branchless "::" detection — no UB */
		has_double_colon |= (is_colon & prev_was_colon);
		prev_was_colon = is_colon;

		int is_dot = (c == '.');
		dot_cnt += is_dot;
		last_dot = is_dot ? (int)i : last_dot;

		int is_hex =
			((c >= '0') & (c <= '9')) |
			((c >= 'a') & (c <= 'f')) |
			((c >= 'A') & (c <= 'F'));

		int is_punct = is_dot | is_colon | (c == '%');

		invalid_char |= !(is_hex | is_punct);
	}

	if (invalid_char)
		return -1;

	/* Pure IPv4 */
	if (colon_cnt == 0)
		return (dot_cnt == 3) ? 0 : -1;

	/* IPv4 with port */
	if (dot_cnt == 3 && colon_cnt == 1 && last_colon > last_dot)
		return 0;

	/* IPv6 full (7 colons, no dots) */
	if (colon_cnt == 7 && dot_cnt == 0)
		return 1;

	/* IPv6 compressed (::) */
	if (has_double_colon)
		return 1;

	/* IPv6 simple (no dots, no ::, fewer than 7 colons) */
	if (dot_cnt == 0)
		return 1;

	/* IPv6-mapped IPv4 (::ffff:a.b.c.d) */
	if (has_double_colon && dot_cnt == 3)
		return 1;

	/* Mixed: a.b.c.d:hextet:... → IPv6 */
	if (dot_cnt == 3 && colon_cnt > 1)
		return 1;

	return -1;
}


/* ---------------------------------------------------------------------------
 *  IP Anonymization (AVX2)
 *
 *  Masks IPv4/IPv6 addresses with 'x'. Supports RFC 4291, CIDR, zones,
 *  and port suffixes. Processing is zero-allocation.
 *
 *  @return: Pointer to static TLS buffer (ctx->ipdest).
 *           DO NOT free() or store for cross-thread use.
 *
 *  @note:   Input max length is 79 chars (MAX_IPV6_LEN).
 *  @hw:     Requires AVX2. No fallback needed on modern x86-64 CPUs.
 * --------------------------------------------------------------------------- */

static inline char* AnonIp(const char* ip, int ip_anon_lvl, uint8_t isIpv6, ThreadContext* ctx)
{
	char* dst = ctx->ipdest;

	/* ---------- IPv4 (scalar, fast) ----------
	 Loop until the first suffix delimiter character:
	*   ':' = port (e.g., 192.168.1.1:8080)
	*   '/' = CIDR (e.g., 192.168.1.1/24)
	*   '%' = zone ID (e.g., 192.168.1.1%eth0) — rare in IPv4 but tolerated
	*   '[' / ']' = IPv6 bracket (unexpected in IPv4, but for safety)
	*
	* Condition 'ip[len]' also checks the NUL terminator (safety)
	*/
	if (!isIpv6) {
		size_t len = 0;
		while (ip[len] &&
			ip[len] != ':' &&
			ip[len] != '/' &&
			ip[len] != '%' &&
			ip[len] != '[' &&
			ip[len] != ']') {
			++len;
		}
		if (len > 80) len = 80;

		memcpy(dst, ip, len);
		dst[len] = '\0';

		int bytes_to_mask = ip_anon_lvl;
		if (bytes_to_mask < 0) bytes_to_mask = 0;
		if (bytes_to_mask > 4) bytes_to_mask = 4;

		/* If nothing to mask, copy suffix and exit (never there, redundant security) */
		if (bytes_to_mask == 0) {
			size_t suffix_len = strlen(ip + len);
			if (len + suffix_len >= sizeof(ctx->ipdest))
				suffix_len = sizeof(ctx->ipdest) - len - 1;
			memcpy(dst + len, ip + len, suffix_len + 1);
			return dst;
		}


		/* --- Calculate where to start masking (mask_start) ---
		* Example: IP "192.168.1.42", bytes_to_mask = 2 (ANON_IP_2)
		*   Goal: keep 4-2=2 bytes visible → "192.168.x.x"
		*   Strategy: skip the first 2 bytes (up to the 2nd dot)
		*   mask_start = position of the 3rd byte (after the 2nd '.')
		*
		* Special case ANON_IP_4 : target=0 → mask_start=0 (mask everything)
		*/

		int dot_count = 0;
		size_t mask_start = 0;
		int target = 4 - bytes_to_mask;

		/* Find mask_start, then BREAK */
		for (size_t i = 0; i < len; ++i) {
			if (ip[i] == '.') {
				dot_count++;
				if (dot_count == target) {
					mask_start = i + 1;
					break;
				}
			}
		}
		/*  ANON_IP_4 : mask everything */
		if (target == 0) mask_start = 0;  

		/* Apply the mask */
		for (size_t i = mask_start; i < len; ++i) {
			if (dst[i] != '.')
				dst[i] = 'x';
		}

		size_t suffix_len = strlen(ip + len);
		if (len + suffix_len >= sizeof(ctx->ipdest))
			suffix_len = sizeof(ctx->ipdest) - len - 1;
		memcpy(dst + len, ip + len, suffix_len + 1);
		return dst;
	}

	/* ---------- IPv6 (AVX2) ---------- */

	const size_t max_addr_len = 80;
	size_t src_len = 0;
	size_t addr_end = 0;

	/* --- Collect hextet offsets for :: expansion ---
	* Sep_off[] stores the START positions of each hextet.
	* Ex: "2001:0db8:85a3::8a2e:0370:7334"
	*     sep_off[0]=0 ('2'), sep_off[1]=':', sep_off[2]='8', etc.
	*
	* Max size 17 = 8 hextets + 7 separators (:) + 1 start = 16, margin 17.
	*/

	size_t sep_off[17];
	int sep_cnt = 0;
	int double_colon_idx = -1;
	int seen_double_colon = 0;

	sep_off[sep_cnt++] = 0;


	/* --- Scan the IPv6 string ---
	 * Goal: determine 'addr_end' (end of the address, before suffix)
	 * and build 'sep_off' for later :: expansion.
	 */

	while (ip[src_len] != '\0') {
		char c = ip[src_len];

		if (c == '[' || c == ']' || c == '/' || c == '%') {
			addr_end = src_len;
			break;
		}
		else if (c == ':') {
			if (src_len > 0 && ip[src_len - 1] == ':') {
				if (seen_double_colon) return NULL;
				seen_double_colon = 1;
				double_colon_idx = sep_cnt - 1;
				if (sep_cnt >= 17) return NULL;
				sep_off[sep_cnt++] = src_len + 1;
			}
			else {
				if (sep_cnt >= 17) return NULL;
				sep_off[sep_cnt++] = src_len + 1;
			}
		}

		++src_len;
		if (src_len >= max_addr_len) break;
	}

	if (addr_end == 0) addr_end = src_len;
	if (addr_end > max_addr_len) addr_end = max_addr_len;

	/* ---  Expansion of '::' (compression IPv6) ---
	 *  IPv6 requires 8 hextets. '::' represents the missing zeros.
	 *
	 * Ex: "2001:db8::1" → 3 explicit hextets + :: + 1 hextet = 4 hextets
	 *   Missing hextets = 8 - 4 = 4 hextets of zeros to insert.
	 *
	 * Method: shift sep_off[] after '::' and insert the virtual offsets.
	 */

	if (seen_double_colon) {
		int explicit_hextets = sep_cnt;
		int missing_hextets = 8 - explicit_hextets;
		if (missing_hextets < 0) return NULL;

		/* Phase 1 : shift the hextets AFTER '::' to the right */
		if (missing_hextets > 0) {
			for (int i = sep_cnt - 1; i > double_colon_idx; --i)
				sep_off[i + missing_hextets] = sep_off[i];

			/* Phase 2 : insert the virtual offsets of the missing hextets */
			for (int i = 0; i < missing_hextets; ++i)
				sep_off[double_colon_idx + i + 1] = sep_off[double_colon_idx] + i + 1;

			sep_cnt += missing_hextets;
		}
	}

	int max_units = 8;
	int units_to_mask = ip_anon_lvl * 2;
	if (units_to_mask < 0) units_to_mask = 0;
	if (units_to_mask > max_units) units_to_mask = max_units;
	if (units_to_mask > sep_cnt) units_to_mask = sep_cnt;

	size_t mask_start = (units_to_mask == 0)
		? addr_end
		: sep_off[sep_cnt - units_to_mask];

	if (mask_start > addr_end) mask_start = addr_end;

	size_t suffix_len = strlen(ip + addr_end);
	if (addr_end + suffix_len >= sizeof(ctx->ipdest))
		suffix_len = sizeof(ctx->ipdest) - addr_end - 1;

	memcpy(dst, ip, addr_end);

	for (size_t i = mask_start; i < addr_end; ++i) {
		char c = dst[i];
		if (c != ':' && c != '.' && c != '[' && c != ']')
			dst[i] = 'x';
	}

	/* Guarantee NUL termination */
	if (suffix_len > 0) {
		memcpy(dst + addr_end, ip + addr_end, suffix_len + 1);
	}
	else {
		dst[addr_end] = '\0';
	}

	return dst;
}


inline static uint64_t FastSeed(const char* url, size_t len) {
	uint64_t seed = len;
	if (len > 8) {
		seed |= (uint64_t)url[0] << 8;
		seed |= (uint64_t)url[len / 2] << 16;
		seed |= (uint64_t)url[len - 1] << 24;

		uint32_t mid_word;
		memcpy(&mid_word, url + len / 2 - 2, sizeof(mid_word));
		seed ^= (uint64_t)mid_word << 32;
	}
	return seed;
}

// Version with pre-calculated k (log2(size))
static inline uint32_t GetBucketFromHash(uint64_t hash, uint32_t k){
	uint32_t mixed = (uint32_t)hash ^ (uint32_t)(hash >> 32);
	return mixed & ((1U << k) - 1);
}

/*==============================================================================
 * CacheAddOrGet - Combined URL/IP cache lookup and insertion.
 *
 * DESIGN & PERFORMANCE CHOICES:
 * - Unrolled & Scalar Design: Fully unrolled in C without abstraction layers or
 *   generics to allow max compiler optimization, zero indirection, and clean
 *   inlining directly into the thread's hot loop.
 * - 64-Byte Cache Line Alignment: Structs are aligned to 64 bytes to fit L1
 *   cache lines. Given an average probe depth of ~1.21 steps, scalar CPU
 *   execution hits on the 1st/2nd attempt without paying SIMD/AVX setup penalties
 *   or losing cross-architecture portability.
 * - Multi-Hash Cascade: Combines CityHash64 (L1), XXH3_64 (L2), and CityHash^XXH3
 *   (L3) to break primary clustering before triggering a full L4 scan.
 *
 * Parameters:
 *   cacheURL, cacheIP : Pointers to thread-local cache arrays (size MUST be power of 2)
 *   size              : Number of entries in cache arrays
 *   url_param, ip_param: Key string buffers
 *   url_len, ip_len   : Length of key buffers in bytes
 *   k                 : Bucket scaling shift parameter
 *
 * Return:
 *   Packed uint64_t: [ URL Index (32 bits) | IP Index (32 bits) ]
 *   Returns UINT32_MAX in respective 32-bit word if a cache is saturated.
 *============================================================================*/
uint64_t CacheAddOrGet(
	CacheEntryURL* cacheURL, CacheEntryIPV6* cacheIP, uint64_t size, 
	char* url_param, uint32_t url_len, 
	const char* ip_param, uint32_t ip_len,
	const uint32_t k) {

	uint32_t rtn_URL = 0;
	/*
		URL Cache Management
	*/
	{
		uint16_t local_url_len = (uint16_t)url_len; // Includes the '\0'
		uint64_t hash = 0;
		uint64_t hash2 = 0;
		uint64_t hash3 = 0;
		uint32_t bucket = 0;
		hash = cityhash64_with_seed(url_param, local_url_len, FastSeed(url_param, local_url_len));
		bucket = GetBucketFromHash(hash, k);

		const char* key = url_param;
		LOG_DEBUG("CacheAddorGet begin : %s %d", key, (int)local_url_len);
		// 1. Try the primary bucket with CityHash64 + quick probe if necessary
		uint16_t i;
		for (i = 0; i < PROBE_RANGE_PRIMARY; i++) {
			uint32_t idx = (bucket + i) & (size - 1);  // Simple linear probing
			CacheEntryURL* entry = &cacheURL[idx];

			// 1a. Empty slot → Direct insertion
			if (!entry->used_in_shard) {
				// LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(entry->value, key, local_url_len);
				entry->hash = hash;
				entry->used_in_shard = 1;
				entry->value_len = local_url_len;
				rtn_URL = idx;
				STATS_INC64(L1_url_cache_hit);
				goto IP;
			}

			// 2a. Existing key → Update
			if (entry->hash == hash && entry->value[0] == key[0] && entry->value[local_url_len >> 1] == key[local_url_len >> 1]
				&& local_url_len == entry->value_len && memcmp(entry->value, key, local_url_len) == 0) {
				entry->used_in_shard = 1;
				STATS_INC64(L1_url_cache_hit);
				//LOG_TRACE("Cache Update hash/key/heat: %I64d %s %d", hash, key, entry->heat);
				rtn_URL = idx;
				goto IP;
			}
			STATS_INC64(url_cache_probes_total);
		}
		STATS_MAX(url_cache_probes_max, i);

		// 2. Try the secondary bucket with XXH3_64 + quick probe if necessary
		LOG_TRACE("COLLISION...");

		hash2 = XXH3_64bits_withSeed(url_param, local_url_len, FastSeed(url_param, local_url_len));
		bucket = GetBucketFromHash(hash2, k) & (size - 1);

		for (i = 1; i <= PROBE_RANGE_SECONDARY; ++i) {
			uint32_t probe_idx = (bucket + i * 13) & (size - 1);
			CacheEntryURL* probe = &cacheURL[probe_idx];

			// LOG_TRACE("PROBING...");
			// 1a. Empty slot → Direct insertion
			if (!probe->used_in_shard) {
				// LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(probe->value, key, local_url_len);
				probe->hash = hash2;
				probe->value_len = local_url_len;
				probe->used_in_shard = 1;
				rtn_URL = probe_idx;
				STATS_INC64(L2_url_cache_hit);
				goto IP;
			}

			// 2a. Existing key → Update
			if (probe->hash == hash2 && probe->value[0] == key[0] && probe->value[local_url_len >> 1] == key[local_url_len >> 1]
				&& probe->value_len == local_url_len && memcmp(probe->value, key, local_url_len) == 0) {
				probe->used_in_shard = 1;
				STATS_INC64(L2_url_cache_hit);
				rtn_URL = probe_idx;
				goto IP;
			}
			LOG_TRACE("PROBING... new step : %d", i);
			STATS_INC64(url_cache_probes_total);
		}
		STATS_MAX(url_cache_probes_max, i);

		// 3. CityHash64 XOR XXH3_64 for another attempt with extended probing
		hash3 = hash ^ hash2;
		bucket = GetBucketFromHash(hash3, k) & (size - 1);

		for (i = 1; i <= PROBE_RANGE_TERTIARY; ++i) {
			uint32_t probe_idx = (bucket + i * 17) & (size - 1);
			CacheEntryURL* probe = &cacheURL[probe_idx];

			// LOG_TRACE("PROBING...");
			// 1a. Empty slot → Direct insertion
			if (!probe->used_in_shard) {
				// LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(probe->value, key, local_url_len);
				probe->hash = hash3;
				probe->value_len = local_url_len;
				probe->used_in_shard = 1;
				rtn_URL = probe_idx;
				STATS_INC64(L3_url_cache_hit);
				goto IP;
			}

			// 2a. Existing key → Update
			if (probe->hash == hash3 && probe->value[0] == key[0] && probe->value[local_url_len >> 1] == key[local_url_len >> 1]
				&& probe->value_len == local_url_len && memcmp(probe->value, key, local_url_len) == 0) {
				probe->used_in_shard = 1;
				STATS_INC64(L3_url_cache_hit);
				rtn_URL = probe_idx;
				goto IP;
			}

			LOG_TRACE("PROBING... new step : %d", i);
			STATS_INC64(url_cache_probes_total);
		}
		STATS_MAX(url_cache_probes_max, i);

		// 5. Last resort: global scan
		STATS_INC64(url_cache_fullprobescan_total);
		for (i = 0; i < size; ++i) {
			
			if (!cacheURL[i].used_in_shard) {
				memcpy(cacheURL[i].value, key, local_url_len);
				cacheURL[i].hash = hash3;
				cacheURL[i].used_in_shard = 1;
				cacheURL[i].value_len = local_url_len;
				rtn_URL = i;
				goto IP;
			}
			else if (cacheURL[i].hash == hash3 && cacheURL[i].value[0] == key[0] && cacheURL[i].value[local_url_len >> 1] == key[local_url_len >> 1]
				&& cacheURL[i].value_len == local_url_len && memcmp(cacheURL[i].value, key, local_url_len) == 0) {
				cacheURL[i].used_in_shard = 1;
				rtn_URL = i;
				goto IP;
			}
			STATS_INC64(url_cache_probes_total);
		}
		STATS_MAX(url_cache_probes_max, i);

		// Failure (cache saturated)
		LOG_TRACE("Cache saturated");
		// Forced Fail for testing
		//exit(-404);
		rtn_URL = UINT32_MAX;
		goto IP;
	}

	/*
		IP Cache Management
	*/
IP :
	{
		uint64_t hash = 0;
		uint64_t hash2 = 0;
		uint64_t hash3 = 0;
		uint32_t bucket = 0;
		hash = cityhash64_with_seed(ip_param, ip_len, FastSeed(ip_param, ip_len));
		bucket = GetBucketFromHash(hash, k);

		const char* key = ip_param;
		LOG_DEBUG("CacheAddorGet begin : %s %d", key, (int)ip_len);
		// 1. Try the primary bucket with CityHash64 + quick probe if necessary
		uint16_t i;
		for (i = 0; i < PROBE_RANGE_PRIMARY; i++)
		{
			uint32_t idx = (bucket + i) & (size - 1);  // Simple linear probing
			CacheEntryIPV6* entry = &cacheIP[idx];

			// 1a. Empty slot → Direct insertion
			// OK for both IP and URL
			if (!entry->used_in_shard) {
				//LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(entry->value, key, ip_len);
				entry->hash = hash;
				entry->used_in_shard = 1;
				entry->value_len = (uint8_t)ip_len;
				STATS_INC64(L1_ip_cache_hit);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)idx);
			}

			// 2a. Existing key → Update
			if (entry->hash == hash && entry->value[0] == key[0] && entry->value[ip_len >> 1] == key[ip_len >> 1]
				&& ip_len == entry->value_len && memcmp(entry->value, key, ip_len) == 0) {
				entry->used_in_shard = 1;
				STATS_INC64(L1_ip_cache_hit);
				// LOG_TRACE("Cache Update hash/key/heat: %I64d %s %d", hash, key, entry->heat);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)idx);
			}
			STATS_INC64(ip_cache_probes_total);
		}
		STATS_MAX(ip_cache_probes_max, i);

		// 2. Try the secondary bucket with XXH3_64 + quick probe if necessary
		LOG_TRACE("COLLISION...");
		hash2 = XXH3_64bits_withSeed(ip_param, ip_len, FastSeed(ip_param, ip_len));
		bucket = GetBucketFromHash(hash2, k) & (size - 1);

		for (i = 1; i <= PROBE_RANGE_SECONDARY; ++i) {
			uint32_t probe_idx = (bucket + i * 13) & (size - 1);
			CacheEntryIPV6* probe = &cacheIP[probe_idx];

			// LOG_TRACE("PROBING...");
			// 1a. Empty slot → Direct insertion
			if (!probe->used_in_shard) {
				//LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(probe->value, key, ip_len);
				probe->hash = hash2;
				probe->value_len = (uint8_t)ip_len;
				probe->used_in_shard = 1;
				STATS_INC64(L2_ip_cache_hit);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)probe_idx);
			}

			// 2a. Existing key → Update
			if (probe->hash == hash2 && probe->value[0] == key[0] && probe->value[ip_len >> 1] == key[ip_len >> 1]
				&& probe->value_len == ip_len && memcmp(probe->value, key, ip_len) == 0) {
				probe->used_in_shard = 1;
				STATS_INC64(L2_ip_cache_hit);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)probe_idx);
			}

			LOG_TRACE("PROBING... new step : %d", i);
			STATS_INC64(ip_cache_probes_total);
		}
		STATS_MAX(ip_cache_probes_max, i);

		// 3. CityHash64 XOR XXH3_64 for another attempt with extended probing
		hash3 = hash ^ hash2;
		bucket = GetBucketFromHash(hash3, k) & (size - 1);

		for (i = 1; i <= PROBE_RANGE_TERTIARY; ++i) {
			uint32_t probe_idx = (bucket + i * 17) & (size - 1);
			CacheEntryIPV6* probe = &cacheIP[probe_idx];

			// LOG_TRACE("PROBING...");
			// 1a. Empty slot → Direct insertion
			if (!probe->used_in_shard) {
				// LOG_TRACE("Cache Insert hash/key/heat: %I64d %s %d", hash, key, HEAT_INSERT);
				memcpy(probe->value, key, ip_len);
				probe->hash = hash3;
				probe->value_len = (uint8_t)ip_len;
				probe->used_in_shard = 1;
				STATS_INC64(L3_ip_cache_hit);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)probe_idx);
			}

			// 2a. Existing key → Update
			if (probe->hash == hash3 && probe->value[0] == key[0] && probe->value[ip_len >> 1] == key[ip_len >> 1]
				&& probe->value_len == ip_len && memcmp(probe->value, key, ip_len) == 0) {
				probe->used_in_shard = 1;
				STATS_INC64(L3_ip_cache_hit);
				return (((uint64_t)rtn_URL << 32) | (uint64_t)probe_idx);
			}

			LOG_TRACE("PROBING... new step : %d", i);
			STATS_INC64(ip_cache_probes_total);	
		}
		STATS_MAX(ip_cache_probes_max, i);

		// 5. Last resort: global scan
		STATS_INC64(ip_cache_fullprobescan_total);
		for (i = 0; i < size; ++i) {
			if (!cacheIP[i].used_in_shard) {
				memcpy(cacheIP[i].value, key, ip_len);
				cacheIP[i].hash = hash3;
				cacheIP[i].used_in_shard = 1;
				cacheIP[i].value_len = (uint8_t)ip_len;
				return (((uint64_t)rtn_URL << 32) | (uint64_t)i);
			}
			else if (cacheIP[i].hash == hash3 && cacheIP[i].value[0] == key[0] && cacheIP[i].value[ip_len >> 1] == key[ip_len >> 1]
				&& cacheIP[i].value_len == ip_len && memcmp(cacheIP[i].value, key, ip_len) == 0) {
				cacheIP[i].used_in_shard = 1;
				return (((uint64_t)rtn_URL << 32) | (uint64_t)i);
			}
			STATS_INC64(ip_cache_probes_total);
		}
		STATS_MAX(ip_cache_probes_max, i);
		// Failure (cache saturated)
		LOG_TRACE("Cache saturated");
		return (((uint64_t)rtn_URL << 32) | (uint64_t)UINT32_MAX);
	}
}


// =======================================================================
// CompressionTask
// =======================================================================
void CALLBACK CompressionTask(PTP_CALLBACK_INSTANCE instance,
	PVOID context,
	PTP_WORK work)
{
	ThreadContext* ctx = (ThreadContext*)context;
	LOG_DEBUG("CompressionThread begin");

	if (!ctx || !ctx->pending_buffer) {
		goto LEAVE;
	}

	LOG_DEBUG("CompressionThread context OK");

	/* --------------------------------------------------------------
	 * 1 – Calculating the maximum payload size (includes the header)
	 * -------------------------------------------------------------- */
	size_t max_payload = (global_compression_level == COMPRESSION_NONE)
		? ctx->pending_size
		: LZ4_compressBound((int)ctx->pending_size);
	size_t framed_sz = sizeof(BlockHeader) + max_payload;

	/* --------------------------------------------------------------
	 * 2 – Unique allocation (header + payload)
	 * -------------------------------------------------------------- */
	BYTE* frame = (BYTE*)malloc(framed_sz);
	if (!frame) {
		LOG_ERROR("OOM allocating %zu bytes", framed_sz);
		WriteError(herror, "oom allocating frame buffer");
		goto LEAVE;
	}

	BYTE* payload = frame + sizeof(BlockHeader);
	int compressed_size = 0;

	/* --------------------------------------------------------------
	 * 3 – Compression (or raw copy)
	 * -------------------------------------------------------------- */
	if (global_compression_level == COMPRESSION_NONE) {
		memcpy(payload, ctx->pending_buffer, ctx->pending_size);
		compressed_size = (int)ctx->pending_size;
	}
	else {
		int level = (global_compression_level == COMPRESSION_FAST) ? 12 : 1;
		compressed_size = LZ4_compress_fast(
			(const char*)ctx->pending_buffer,
			(char*)payload,
			(int)ctx->pending_size,
			(int)max_payload,
			level);
	}

	LOG_DEBUG("CompressionThread compressed_size (real) : %d", compressed_size);

	if (compressed_size <= 0) {
		LOG_ERROR("LZ4 compression failed (code=%d) for %zu bytes",
			compressed_size, ctx->pending_size);
		free(frame);
		STATS_INC64(compression_failure_total);
		goto LEAVE;
	}

	/* --------------------------------------------------------------
	 * 4 – Writing the header *in-place*
	 * -------------------------------------------------------------- */
	*(BlockHeader*)frame = (BlockHeader){
		.compSize = (uint32_t)compressed_size,
		.decompSize = (uint32_t)ctx->pending_size
	};

	size_t final_framed_size = sizeof(BlockHeader) + (size_t)compressed_size;

	/* --------------------------------------------------------------
	 * 5 – Sending to the writer (the writer frees `frame`)
	 * -------------------------------------------------------------- */
	WriteTask wt = {
		.taskType = TASK_WRITE,
		.data = frame,
		.size = final_framed_size,
		.ctx = ctx
	};
	WritePool_Enqueue(wt);

	/* --------------------------------------------------------------
	 * 7 – Statistics
	 * -------------------------------------------------------------- */
	STATS_ADD64(compression_lz4_ratio_avg,
		(int64_t)((compressed_size / (float)ctx->pending_size) * 10000));
	STATS_INC64(batch_compressed_total);

LEAVE:
	if (ctx && InterlockedDecrement(&ctx->pending_compression) == 0) {
		EnterCriticalSection(&ctx->compression_cs);
		WakeAllConditionVariable(&ctx->pc_complete);
		LeaveCriticalSection(&ctx->compression_cs);
	}
}

// =======================================================================
// Thread Context Management
// =======================================================================

ThreadContext* InitThreadContext() {

	// 1. Retrieve the existing TLS context
	ThreadContext* ctx = (ThreadContext*)TlsGetValue(tls_index);
	LOG_DEBUG("InitThreadContext begin");

	// 2. If the context does not exist, initialize it
	if (ctx == NULL) {
		LOG_DEBUG("Initializing new thread context\n");

		// Memory allocation for ThreadContext structure
		ctx = (ThreadContext*)_aligned_malloc(sizeof(ThreadContext), 64);
		if (!ctx) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR => malloc %d / %llu\n", __LINE__, sizeof(ThreadContext));
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			return NULL;
		}

		// Ensure no uninitialized pointers are lingering
		ZeroMemory(ctx, sizeof(ThreadContext));

		// 3. Initialize basic parameters & Synchronization primitives
		ctx->file_handle = INVALID_HANDLE_VALUE;
		ctx->pending_compression = 0;
		ctx->active_count = 0;
		ctx->pending_write = 0;
		ctx->flush_count = 0;
		InitializeCriticalSection(&ctx->compression_cs);
		InitializeConditionVariable(&ctx->pc_complete);
		InitializeConditionVariable(&ctx->pw_complete);
		InitializeSRWLock(&ctx->file_lock);
		InitializeConditionVariable(&ctx->file_ready_cv);

		// 4. Calculate the buffer capacity (nb d'éléments SerializedEntry)
		ctx->buffer_capacity = g_buffer_size / sizeof(SerializedEntry);
		LOG_DEBUG("InitThreadContext Buffer capacity %zu (buffer size: %u, entry size: %llu)",
			ctx->buffer_capacity, g_buffer_size, sizeof(SerializedEntry));

		// --- Scratch buffers for indexing (Capacity in NUMBER OF ELEMENTS uint32_t) ---
		ctx->nb_urls_dicts = (uint32_t*)_aligned_malloc(ctx->buffer_capacity * sizeof(uint32_t), 32);
		ctx->nb_urls_dicts_size = ctx->buffer_capacity; // Number of elements

		ctx->nb_ips_dicts = (uint32_t*)_aligned_malloc(ctx->buffer_capacity * sizeof(uint32_t), 32);
		ctx->nb_ips_dicts_size = ctx->buffer_capacity; // Number of elements

		// --- Allocation of DICTBUF (Exact worst-case calculation in BYTES) ---
		// Canaries (16B) + Header counts (8B) + (URL_meta + MAX_URL_LEN + IP_meta + IP_SIMD_LEN) * capacity + align (32B)
		ctx->dictbuf_size = sizeof(DICT_BEGIN) + sizeof(DICT_END) + (sizeof(uint32_t) * 2) +
			ctx->buffer_capacity * (sizeof(uint32_t) + sizeof(uint16_t) + MAX_URL_LEN +
				sizeof(uint32_t) + sizeof(uint16_t) + 96) + 32;

		ctx->dictbuf = _aligned_malloc(ctx->dictbuf_size, 32);

		// --- Allocation of DATABUF (Exact size in BYTES) ---
		ctx->databuf_size = ctx->buffer_capacity * sizeof(SerializedEntry); // Equivalent to g_buffer_size
		ctx->databuf = _aligned_malloc(ctx->databuf_size, 32);

		// --- Allocation of PENDING_BUFFER (Exact size in BYTES) ---
		ctx->pending_buffer_size = ctx->databuf_size + ctx->dictbuf_size;
		ctx->pending_buffer = _aligned_malloc(ctx->pending_buffer_size, 32);

		// Global check of buffer allocations
		if (!ctx->pending_buffer || !ctx->databuf || !ctx->dictbuf ||
			!ctx->nb_urls_dicts || !ctx->nb_ips_dicts) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR => global malloc %d\n", __LINE__);
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			goto FREE;
		}

		// 5. Allocate the active buffer
		ctx->active_buffer = (SerializedEntry*)_aligned_malloc(g_buffer_size, 32);
		if (!ctx->active_buffer) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR => malloc %d : %lu / %u\n", __LINE__, GetLastError(), g_buffer_size);
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			goto FREE;
		}

		// 6. Initialize caches
		ctx->url_cache = (CacheEntryURL*)_aligned_malloc(g_cache_size * sizeof(CacheEntryURL), 32);
		if (!ctx->url_cache) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR=> malloc %d / %llu\n", __LINE__, g_cache_size * sizeof(CacheEntryURL));
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			goto FREE;
		}

		ctx->ipv6_cache = (CacheEntryIPV6*)_aligned_malloc(g_cache_size * sizeof(CacheEntryIPV6), 32);
		if (!ctx->ipv6_cache) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR=> malloc %d / %llu\n", __LINE__, g_cache_size * sizeof(CacheEntryIPV6));
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			goto FREE;
		}

		ZeroMemory(ctx->url_cache, g_cache_size * sizeof(CacheEntryURL));
		ZeroMemory(ctx->ipv6_cache, g_cache_size * sizeof(CacheEntryIPV6));

		LOG_DEBUG("InitThreadContext Cache capacity (cache size: %zu, entry size: %zu)", g_cache_size, sizeof(CacheEntryURL));

		// 7. Register in TLS
		if (!TlsSetValue(tls_index, ctx)) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR => tls %d / %lu\n", __LINE__, GetLastError());
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
			goto FREE;
		}
		LOG_DEBUG("TLS Init OK");

		// Add to the global list
		ContextList* new_node = (ContextList*)malloc(sizeof(ContextList));
		if (!new_node) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ITERROR => malloc %d / %llu\n", __LINE__, sizeof(ContextList));
			WriteError(herror, msg);
			goto FREE;
		}
		new_node->ctx = ctx;
		LOG_DEBUG("Context List OK");

		EnterCriticalSection(&context_list_lock);
		new_node->next = global_context_list;
		global_context_list = new_node;
		LeaveCriticalSection(&context_list_lock);

		// Create the initial log file
		WriteTask t = {
			.ctx = ctx,
			.data = NULL,
			.file_handle = INVALID_HANDLE_VALUE,
			.size = 0,
			.taskType = TASK_CREATE_OR_ROTATE
		};
		WritePool_Enqueue(t);

		LOG_DEBUG("Thread context initialized for TID %lu\n", GetCurrentThreadId());
	}

	return ctx;

FREE:
	if (ctx) {
		if (ctx->pending_buffer) _aligned_free(ctx->pending_buffer);
		if (ctx->databuf)        _aligned_free(ctx->databuf);
		if (ctx->dictbuf)        _aligned_free(ctx->dictbuf);
		if (ctx->nb_urls_dicts)  _aligned_free(ctx->nb_urls_dicts);
		if (ctx->nb_ips_dicts)   _aligned_free(ctx->nb_ips_dicts);
		if (ctx->active_buffer)  _aligned_free(ctx->active_buffer);
		if (ctx->url_cache)      _aligned_free(ctx->url_cache);
		if (ctx->ipv6_cache)     _aligned_free(ctx->ipv6_cache);
		DeleteCriticalSection(&ctx->compression_cs);
		_aligned_free(ctx);
	}
	return NULL;
}



// One‐time callback that performs all the global setup exactly once
BOOL CALLBACK InitializeOnce(PINIT_ONCE initOnce, PVOID parameter, LPVOID* rtnctx) {

	UNREFERENCED_PARAMETER(initOnce);
	UNREFERENCED_PARAMETER(rtnctx);

	_LogInitParams const* pInit = (_LogInitParams*)parameter;

	// 1. Copy and truncate paths
	strncpy_s(base_path, MAX_PATH, pInit->log_path, _TRUNCATE);
	strncpy_s(error_path, MAX_PATH, pInit->error_path, _TRUNCATE);
	strncpy_s(backup_path, MAX_PATH, pInit->backup_path, _TRUNCATE);

	// 2. Create target directories
	char pathes[3][MAX_PATH];
	strcpy_s(pathes[0], MAX_PATH, base_path);
	strcpy_s(pathes[1], MAX_PATH, error_path);
	strcpy_s(pathes[2], MAX_PATH, backup_path);

	for (int i = 0; i < _countof(pathes); i++) {
		HRESULT hr = SHCreateDirectoryExA(NULL, pathes[i], NULL);
		if (hr != ERROR_SUCCESS && hr != ERROR_ALREADY_EXISTS) {
			// As a last resort if the directory does not exist
			OutputDebugStringA("LIOERROR => Failed to create log directories\n");
			return FALSE;
		}
	}

	use_backup_path = 0;

	// 3. Initialize the error log file
	herror = CreateErrorFile(error_path);
	if (herror == INVALID_HANDLE_VALUE || herror == NULL) {
		OutputDebugStringA("LIOERROR => Failed to create error log file\n");
		return FALSE;
	}

	// 4. Allocate the TLS Slot
	tls_index = TlsAlloc();
	if (tls_index == TLS_OUT_OF_INDEXES) {
		char msg[256];
		snprintf(msg, sizeof(msg), "LIOERROR => TLS allocation failed at line %d\n", __LINE__);
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		return FALSE;
	}

	InitGlobalConfig();

	// Initialize sizes & flags
	global_write_pool.queue_size = g_write_queue_size;
	global_write_pool.threads_count = g_write_pool_thread_count;
	global_sequence = 0;
	file_counter = 0;
	enable_sequence = (pInit->enableSeqFlag != 0);
	flush_per_file = pInit->flush_per_file;
	global_context_list = NULL;
	global_compression_level = (uint8_t)pInit->clvl;
	truncate_url_params = pInit->truncate_url_params;

	// 6. Calculate buffer sizes (enum -> bytes)
	switch (pInit->bufferSizeLevel) {
	case BATCH_500KB: g_buffer_size = (500U * 1024); break;
	case BATCH_1MB:   g_buffer_size = (1U * 1024 * 1024); break;
	case BATCH_2MB:   g_buffer_size = (2U * 1024 * 1024); break;
	case BATCH_4MB:   g_buffer_size = (4U * 1024 * 1024); break;
	case BATCH_8MB:   g_buffer_size = (8U * 1024 * 1024); break;
	case BATCH_16MB:  g_buffer_size = (16U * 1024 * 1024); break;
	case BATCH_32MB:  g_buffer_size = (32U * 1024 * 1024); break;
	case BATCH_64MB:  g_buffer_size = (64U * 1024 * 1024); break;
	case BATCH_128MB: g_buffer_size = (128U * 1024 * 1024); break;
	case BATCH_256MB: g_buffer_size = (256U * 1024 * 1024); break;
	case BATCH_512MB: g_buffer_size = (512U * 1024 * 1024); break;
	default:          g_buffer_size = (8U * 1024 * 1024); break;
	}

	// 7. Calculate cache sizes
	switch (pInit->cache_size) {
	case DICT_16K:  g_cache_size = (16U * 1024); break;
	case DICT_32K:  g_cache_size = (32U * 1024); break;
	case DICT_64K:  g_cache_size = (64U * 1024); break;
	case DICT_128K: g_cache_size = (128U * 1024); break;
	case DICT_256K: g_cache_size = (256U * 1024); break;
	case DICT_512K: g_cache_size = (512U * 1024); break;
	case DICT_1M:   g_cache_size = (1U * 1024 * 1024); break;
	case DICT_2M:   g_cache_size = (2U * 1024 * 1024); break;
	case DICT_4M:   g_cache_size = (4U * 1024 * 1024); break;
	case DICT_8M:   g_cache_size = (8U * 1024 * 1024); break;
	case DICT_16M:  g_cache_size = (16U * 1024 * 1024); break;
	default:        g_cache_size = (256U * 1024); break;
	}

	g_log2_64 = Log2_64(g_cache_size);

	// 8. Initialize asynchronous pools (Writing and Compression)
	WritePool_Init();

	g_threadPool = CreateThreadpool(NULL);
	if (!g_threadPool) {
		char msg[256];
		snprintf(msg, sizeof(msg), "LIOERROR => CreateThreadpool failed line %d (%lu)\n", __LINE__, GetLastError());
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		return FALSE;
	}

	// 9. Set the IP anonymization level
	ip_anon_lvl = pInit->ip_anon_lvl;

	SetThreadpoolThreadMaximum(g_threadPool, (g_write_pool_thread_count <= 6) ? 6 : g_write_pool_thread_count);
	SetThreadpoolThreadMinimum(g_threadPool, MIN_COMPRESSORS);

	InitializeThreadpoolEnvironment(&g_envCompress);
	SetThreadpoolCallbackPool(&g_envCompress, g_threadPool);
	SetThreadpoolCallbackPriority(&g_envCompress, TP_CALLBACK_PRIORITY_HIGH);

	return TRUE;
}



// ==============================================================================
// Public API
// ==============================================================================

/*==============================================================================
 * PulpInit - Public initialization entry point.
 * Flow: 1. Hard guards -> 2. Paths/Flags norm -> 3. Autoconf -> 4. Bounds check
 *============================================================================*/
uint8_t PulpInit(const char* log_path, const char* backup_log_path, const char* err_path,
	uint8_t enable_seq, Anon_lvl anon_lvl, uint8_t trunc_url_params,
	uint16_t nb_flush_per_file, Lz4CompressionLevel clvl,
	BatchSize buffer_size, DictSize cache_size) {

	uint8_t rtn_val = RTN_OK;

	// 1. Guard clauses
	if (log_path == NULL || strcmp(log_path, "NUL") == 0)
		return RTN_INIT_FAIL_MISSING_PARAM;

	// 2. Path normalization and secondary parameters
	if (err_path == NULL)        err_path = "error";
	if (backup_log_path == NULL) backup_log_path = "";

	if (strlen(log_path) > MAX_PATH || strlen(err_path) > MAX_PATH || strlen(backup_log_path) > MAX_PATH)
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;

	if (enable_seq != 0 && enable_seq != 1) {
		enable_seq = 1;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
	}
	if (trunc_url_params != 0 && trunc_url_params != 1) {
		trunc_url_params = 1;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
	}
	if (anon_lvl < ANON_IP_NONE || anon_lvl > ANON_IP_4) {
		anon_lvl = IP_ANON_DEFAULT;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
	}
	if (nb_flush_per_file == 0 || (nb_flush_per_file & (nb_flush_per_file - 1)) != 0) {
		nb_flush_per_file = FLUSH_PER_FILE_DEFAULT;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
	}
	if (clvl < COMPRESSION_FAST || clvl > COMPRESSION_NONE) {
		clvl = COMPRESSION_LEVEL_DEFAULT;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
	}

	// 3. Hardware autoconfiguration (Pair Dict / Batch indissociable)
	if (buffer_size == BATCH_AUTOSIZE || cache_size == DICT_AUTOSIZE) {
		ULONGLONG ram_kb = 0;

		if (!GetPhysicallyInstalledSystemMemory(&ram_kb) || ram_kb == 0) {
			goto DEFAULT_CONFIG;
		}

		ULONGLONG ram_mb = ram_kb / 1024;
		ULONGLONG calculated_ratio = ram_mb / 32;

		if (calculated_ratio < 32) { cache_size = DICT_16K;   buffer_size = BATCH_500KB; }
		else if (calculated_ratio < 64) { cache_size = DICT_32K;   buffer_size = BATCH_1MB; }
		else if (calculated_ratio < 128) { cache_size = DICT_64K;   buffer_size = BATCH_2MB; }
		else if (calculated_ratio < 256) { cache_size = DICT_128K;  buffer_size = BATCH_4MB; }
		else if (calculated_ratio < 512) { cache_size = DICT_256K;  buffer_size = BATCH_8MB; }
		else if (calculated_ratio < 1024) { cache_size = DICT_512K;  buffer_size = BATCH_16MB; }
		else if (calculated_ratio < 2048) { cache_size = DICT_1M;    buffer_size = BATCH_32MB; }
		else if (calculated_ratio < 4096) { cache_size = DICT_2M;    buffer_size = BATCH_64MB; }
		else if (calculated_ratio < 8192) { cache_size = DICT_4M;    buffer_size = BATCH_128MB; }
		else if (calculated_ratio < 16384) { cache_size = DICT_8M;    buffer_size = BATCH_256MB; }
		else { cache_size = DICT_16M;   buffer_size = BATCH_512MB; }

		goto INITIALIZE;

	DEFAULT_CONFIG:
		cache_size = DICT_256K;
		buffer_size = BATCH_8MB;
		rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;

		const char* msg = "LIACWARN => fallback to default values (dict: 256K / batch: 8MB)\n";
		fprintf(stderr, "%s", msg);
		OutputDebugStringA(msg);
	}
	else {
		// 4. Normalization of explicit values (Unified validation)
		if (cache_size < DICT_16K || cache_size > DICT_16M) {
			cache_size = DICT_256K;
			rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
		}
		if (buffer_size < BATCH_500KB || buffer_size > BATCH_512MB) {
			buffer_size = BATCH_8MB;
			rtn_val = RTN_INIT_WARN_DEFAULT_PARAM;
		}
	}

INITIALIZE:
	// 5. Unique thread-safe execution
	_LogInitParams initParams = {
		.log_path = log_path,
		.backup_path = backup_log_path,
		.error_path = err_path,
		.enableSeqFlag = enable_seq,
		.ip_anon_lvl = anon_lvl,
		.flush_per_file = nb_flush_per_file,
		.clvl = clvl,
		.bufferSizeLevel = buffer_size,
		.cache_size = cache_size,
		.truncate_url_params = trunc_url_params
	};

	if (!InitOnceExecuteOnce(&g_loggerInitOnce, InitializeOnce, &initParams, NULL)) {
		const char* msg = "LIERROR => InitOnceExecuteOnce failed\n";
		fprintf(stderr, "%s", msg);
		OutputDebugStringA(msg);
		return RTN_INTERNAL_FAILURE_0;
	}

	return rtn_val;
}


/*==============================================================================
 * PulpWrite - Hot-path entry point for logging a single log/event transaction.
 *
 * Orchestrates the full logging pipeline for one request:
 * 1. Input sanitization (URL truncation / ellipsis)
 * 2. IP copying / AVX anonymization (with raw fallback on malformed IPs)
 * 3. URL & IP caching with auto-resilience (dedup & saturation recovery)
 * 4. Per-thread active-buffer append (lock-free TLS)
 * 5. Progressive backpressure on write-queue congestion
 * 6. Triggerable batch flush when active buffer reaches capacity
 *
 * THREAD SAFETY:
 * - Fully re-entrant. Hot-path state is maintained in Thread-Local Storage.
 * - Shared structures use atomic instructions or read-mostly operations.
 *
 * RETURN VALUE (uint16_t):
 * High byte (rtn >> 8) : System error or LogFlush() status code.
 * Low byte  (rtn & 0xFF): Backpressure level or RTN_LOG_REFUSED status.
 *============================================================================*/
uint16_t PulpWrite(
	uint8_t operation_id,
	const char* url, uint32_t url_len,
	uint32_t http_code,
	const char* ip, uint32_t ip_len,
	uint16_t duration_ms,
	uint16_t response_size,
	uint8_t flags,
	uint64_t timestamp)
{
	__declspec(align(32)) char local_url[MAX_URL_LEN + 1] = { 0 };
	uint16_t local_url_len = 0;
	uint8_t rtn_code_hi = 0; // System / Flush status
	uint8_t rtn_code_lo = 0; // Performance & pressure telemetry

	STATS_INC64(log_processed_total);

	// -------------------------------------------------------------------------
	// 1. Sanitize & Prepare URL
	// -------------------------------------------------------------------------
	if (url && url_len > 0) {
		const char* src = url;
		uint32_t src_len = url_len;

		if (truncate_url_params) {
			const char* q = (const char*)memchr(url, '?', url_len);
			if (q) {
				src_len = (uint32_t)(q - url + 1);
			}
		}

		if (src_len < MAX_URL_LEN) {
			memcpy(local_url, src, src_len);
			local_url[src_len] = '\0';
			local_url_len = (uint16_t)src_len + 1;
		}
		else {
			// Truncate long URLs: prefix...suffix
			memcpy(local_url, src, URL_PREFIX_LEN);
			memcpy(local_url + URL_PREFIX_LEN, "...", 3);
			memcpy(local_url + URL_PREFIX_LEN + 3,
				src + src_len - URL_SUFFIX_LEN,
				URL_SUFFIX_LEN);
			local_url[URL_PREFIX_LEN + 3 + URL_SUFFIX_LEN] = '\0';
			local_url_len = MAX_URL_LEN + 1;
		}
	}
	else {
		memcpy(local_url, "NULL", 5);
		local_url_len = 5;
	}

	// -------------------------------------------------------------------------
	// 2. TLS Context Initialization
	// -------------------------------------------------------------------------
	ThreadContext* ctx = InitThreadContext();
	if (!ctx || !ctx->active_buffer) {
		char msg[256];
		snprintf(msg, sizeof(msg), "ITERROR => thread context/buffer failure %d / %lu\n",
			__LINE__, GetLastError());
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		rtn_code_hi = (!ctx) ? RTN_INTERNAL_FAILURE_0 : RTN_INTERNAL_FAILURE_1;
		return (((uint16_t)rtn_code_hi << 8) | (uint16_t)rtn_code_lo);
	}

	// -------------------------------------------------------------------------
	// 3. Sequence & IP Processing
	// -------------------------------------------------------------------------
	uint64_t seq = enable_sequence ? (uint64_t)InterlockedIncrement64(&global_sequence) : 0;

	if (ip_anon_lvl != ANON_IP_NONE) {
		int8_t isipv6 = ClassifyIpFast(ip, ip_len);
		if (isipv6 >= 0) {
			AnonIp(ip, ip_anon_lvl, isipv6, ctx);
		}
		else {
			// Fallback if the IP is malformed/unrecognized: secure raw copy
			if (ip && ip_len > 0) {
				uint32_t copy_len = (ip_len < 95) ? ip_len : 95;
				memcpy(ctx->ipdest, ip, copy_len);
				ctx->ipdest[copy_len] = '\0';
			}
			else {
				ctx->ipdest[0] = '\0';
			}
		}
	}
	else {
		// Normal case: Secure bounded copy to prevent overflow with non-null-terminated strings
		if (ip && ip_len > 0) {
			uint32_t copy_len = (ip_len < 95) ? ip_len : 95;
			memcpy(ctx->ipdest, ip, copy_len);
			ctx->ipdest[copy_len] = '\0';
		}
		else {
			ctx->ipdest[0] = '\0';
		}
	}

	// -------------------------------------------------------------------------
	// 4. Cache Insertion & Saturation Resilience
	// -------------------------------------------------------------------------
	uint8_t retry = 0;

RETRY:
	// Direct 64-bit aligned atomic read on x64
	uint64_t current_cache_size = g_cache_size;

	uint64_t packed = CacheAddOrGet(
		ctx->url_cache, ctx->ipv6_cache, current_cache_size,
		local_url, local_url_len, ctx->ipdest, (uint32_t)strlen(ctx->ipdest), g_log2_64);

	uint32_t url_idx = (uint32_t)(packed >> 32);
	uint32_t ip_idx = (uint32_t)(packed & 0xFFFFFFFF);

	// Handle cache dictionary saturation
	if (url_idx == UINT32_MAX || ip_idx == UINT32_MAX) {
		char msg[256];
		snprintf(msg, sizeof(msg), "LWWARN => dictionary full %d / batchcapacity %llu / gbatchsize %u / THID %lu\n",
			__LINE__, ctx->buffer_capacity, g_buffer_size, GetCurrentThreadId());
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);

		// Force buffer flush and dynamically adapt thread capacity
		PulpFlush(ctx);
		Sleep(0);
		ctx->buffer_capacity = g_cache_size;

		if (++retry <= 3) {
			goto RETRY;
		}
		else {
			goto REFUSED;
		}
	}

	// -------------------------------------------------------------------------
	// 5. Append Record to Thread Active Buffer
	// -------------------------------------------------------------------------
	if (ctx->active_count < ctx->buffer_capacity) {
		SerializedEntry* entry = &ctx->active_buffer[ctx->active_count++];
		entry->sequence = seq;
		entry->timestamp = timestamp;
		entry->url_idx = url_idx;
		entry->ip_idx = ip_idx;
		entry->http_code = (uint16_t)http_code;
		entry->http_verb = operation_id;
		entry->duration_ms = duration_ms;
		entry->flags = flags;
		entry->response_size = response_size;
	}

	// -------------------------------------------------------------------------
	// 6. Progressive Backpressure Mechanism
	// -------------------------------------------------------------------------
	uint32_t queue_depth = (global_write_pool.head - global_write_pool.tail) & (g_write_queue_size - 1);

	if (queue_depth >= (HI_PRESSURE * g_write_queue_size)) {
		Sleep(20);
		rtn_code_lo = RTN_HEAVY_PRESSURE;
		STATS_INC64(backpressure_count);
	}
	else if (queue_depth >= (MED_PRESSURE * g_write_queue_size)) {
		SwitchToThread();
		rtn_code_lo = RTN_MEDIUM_PRESSURE;
		STATS_INC64(backpressure_count);
	}
	else if (queue_depth >= (LO_PRESSURE * g_write_queue_size)) {
		uint8_t spin = 0;
		while ((((global_write_pool.head - global_write_pool.tail) & (g_write_queue_size - 1)) >=
			(LO_PRESSURE * g_write_queue_size)) && (spin < SPIN_LIMIT))
		{
			spin++;
			_mm_pause();
		}
		rtn_code_lo = RTN_LIGHT_PRESSURE;
		STATS_INC64(backpressure_count);
	}

	// -------------------------------------------------------------------------
	// 7. Automatic Buffer Flush Trigger
	// -------------------------------------------------------------------------
	if (ctx->active_count >= ctx->buffer_capacity) {
		STATS_INC64(batch_flushed_total);
		rtn_code_hi = PulpFlush((void*)NULL);
	}

	return (((uint16_t)rtn_code_hi << 8) | (uint16_t)rtn_code_lo);

REFUSED:
	{
		char msg[256];
		snprintf(msg, sizeof(msg), "LWERROR => log refused %d / %llu / %u / %u\n",
			__LINE__, g_cache_size, g_log2_64, g_buffer_size);
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		STATS_INC64(log_refused_total);
		rtn_code_lo = RTN_LOG_REFUSED;
		return (((uint16_t)rtn_code_hi << 8) | (uint16_t)rtn_code_lo);
	}
}

/*==============================================================================
 * PulpGetStats - Generates a JSON snapshot of live system telemetry.
 *
 * Measures processing throughput over a ~2s sampling window and formats L1/L2/L3
 * cache hit ratios, queue depth, compression metrics, and error counters.
 *
 * NOTE:
 * - Blocks the calling thread for ~2s during sampling.
 * - Caller is responsible for freeing the returned string via PulpFreeStats().
 *============================================================================*/
char* PulpGetStats() {
#if PULP_STAT_ACTIVE
	// 1. Take initial sample for throughput calculation
	ULONGLONG t0 = GetTickCount64();
	uint64_t log_total0 = g_stats.log_processed_total;

	// Sampling window (~2 seconds)
	Sleep(1990);

	// 2. Take final snapshot of all counters atomically aligned
	ULONGLONG t1 = GetTickCount64();
	uint64_t log_total = g_stats.log_processed_total;
	uint64_t l1_url = g_stats.L1_url_cache_hit;
	uint64_t l2_url = g_stats.L2_url_cache_hit;
	uint64_t l3_url = g_stats.L3_url_cache_hit;
	uint64_t l1_ip = g_stats.L1_ip_cache_hit;
	uint64_t l2_ip = g_stats.L2_ip_cache_hit;
	uint64_t l3_ip = g_stats.L3_ip_cache_hit;
	uint64_t url_probes = g_stats.url_cache_probes_total;
	uint64_t ip_probes = g_stats.ip_cache_probes_total;
	uint64_t url_probes_max = g_stats.url_cache_probes_max;
	uint64_t ip_probes_max = g_stats.ip_cache_probes_max;
	uint64_t url_fullscan = g_stats.url_cache_fullprobescan_total;
	uint64_t ip_fullscan = g_stats.ip_cache_fullprobescan_total;
	uint64_t batch_flushed = g_stats.batch_flushed_total;
	uint64_t batch_compressed = g_stats.batch_compressed_total;
	uint64_t batch_written = g_stats.batch_written_total;
	uint64_t waitfile_max = g_stats.writer_waitfile_max;
	uint64_t backpressure = g_stats.backpressure_count;
	uint64_t compression_avg = g_stats.compression_lz4_ratio_avg;
	uint64_t compression_fail = g_stats.compression_failure_total;
	uint64_t lost_logs = g_stats.lost_logs_total;
	uint64_t rotation = g_stats.log_rotation;
	uint64_t refused = g_stats.log_refused_total;
	uint64_t resync = g_stats.rotation_resync_total;

	// 3. Compute derived ratios based on the SNAPSHOT values
	float url_L1_percent = (log_total > 0) ? (float)(l1_url * 100) / (float)log_total : 0.0f;
	float url_L2_percent = (log_total > 0) ? (float)(l2_url * 100) / (float)log_total : 0.0f;
	float url_L3_percent = (log_total > 0) ? (float)(l3_url * 100) / (float)log_total : 0.0f;

	float ip_L1_percent = (log_total > 0) ? (float)(l1_ip * 100) / (float)log_total : 0.0f;
	float ip_L2_percent = (log_total > 0) ? (float)(l2_ip * 100) / (float)log_total : 0.0f;
	float ip_L3_percent = (log_total > 0) ? (float)(l3_ip * 100) / (float)log_total : 0.0f;

	float url_insert_ratio = (url_probes > 0) ? (float)(l1_url + l2_url + l3_url) / (float)url_probes : 0.0f;
	float ip_insert_ratio = (ip_probes > 0) ? (float)(l1_ip + l2_ip + l3_ip) / (float)ip_probes : 0.0f;

	float compression_ratio = (batch_compressed > 0) ? (float)compression_avg / (batch_compressed * 10000.0f) : 0.0f;

	// Exact time delta measurement for throughput
	double elapsed_sec = (double)(t1 - t0) / 1000.0;
	float log_throughput = (elapsed_sec > 0.0 && log_total > log_total0) ?
		(float)(((log_total - log_total0) / elapsed_sec) / 1000000.0) : 0.0f;

	// 4. Format JSON in a single pass using a stack buffer
	char stack_buf[1536];
	int len = snprintf(stack_buf, sizeof(stack_buf),
		"{"
		"\"cache_resource_L1\": %llu, "
		"\"cache_resource_L1_percent\": %.2f, "
		"\"cache_resource_L2\": %llu, "
		"\"cache_resource_L2_percent\": %.2f, "
		"\"cache_resource_L3\": %llu, "
		"\"cache_resource_L3_percent\": %.2f, "
		"\"cache_endpoint_L1\": %llu, "
		"\"cache_endpoint_L1_percent\": %.2f, "
		"\"cache_endpoint_L2\": %llu, "
		"\"cache_endpoint_L2_percent\": %.2f, "
		"\"cache_endpoint_L3\": %llu, "
		"\"cache_endpoint_L3_percent\": %.2f, "
		"\"resource_cache_probes_total\": %llu, "
		"\"endpoint_cache_probes_total\": %llu, "
		"\"resource_insert_to_step_ratio\": %.2f, "
		"\"endpoint_insert_to_step_ratio\": %.2f, "
		"\"resource_probes_depth_max\": %llu, "
		"\"endpoint_probes_depth_max\": %llu, "
		"\"resource_fullprobescan_total\": %llu, "
		"\"endpoint_fullprobescan_total\": %llu, "
		"\"log_processed_total\": %llu, "
		"\"batch_flushed_total\": %llu, "
		"\"batch_compressed_total\": %llu, "
		"\"batch_written_total\": %llu, "
		"\"writer_waitfile_max\": %llu, "
		"\"backpressure_count\": %llu, "
		"\"compression_lz4_ratio_avg\": %.2f, "
		"\"compression_failure_total\": %llu, "
		"\"lost_logs_total\": %llu, "
		"\"log_rotation\": %llu, "
		"\"log_refused_total\": %llu, "
		"\"rotation_resync_total\": %llu, "
		"\"throughput_sec\": %.2f"
		"}",
		l1_url, url_L1_percent,
		l2_url, url_L2_percent,
		l3_url, url_L3_percent,
		l1_ip, ip_L1_percent,
		l2_ip, ip_L2_percent,
		l3_ip, ip_L3_percent,
		url_probes,
		ip_probes,
		url_insert_ratio,
		ip_insert_ratio,
		url_probes_max,
		ip_probes_max,
		url_fullscan,
		ip_fullscan,
		log_total,
		batch_flushed,
		batch_compressed,
		batch_written,
		waitfile_max,
		backpressure,
		compression_ratio,
		compression_fail,
		lost_logs,
		rotation,
		refused,
		resync,
		log_throughput
	);

	if (len < 0 || len >= (int)sizeof(stack_buf)) {
		return NULL;
	}

	return _strdup(stack_buf);
#else
	// Fallback if STAT_LOG_ACTIVE is disabled at compile time
	return _strdup("{\"error\": \"statistics_disabled\"}");
#endif
}

void PulpFreeStats(char* p) {
	if (p)
		free(p);
}

/*==============================================================================
 * BuildDictionaryInMemory - Serializes unique URL and IP strings for a batch.
 *
 * Constructs a contiguous, aligned binary dictionary containing all unique URL
 * and IP strings referenced in the active buffer. Used as the shared dictionary
 * for payload compression.
 *
 * BINARY LAYOUT:
 * [CANARY_BEGIN (8B)]
 * [URL_COUNT (4B)]   [ (URL_ID (4B) | URL_LEN (2B) | URL_DATA (VAR) )... ]
 * [IP_COUNT (4B)]    [ (IP_ID (4B)  | IP_LEN (2B)  | IP_DATA (VAR)  )... ]
 * [CANARY_END (8B)]
 *============================================================================*/
static BYTE* BuildDictionaryInMemory(ThreadContext* ctx, size_t* out_size) {
	const uint64_t canary_begin = DICT_BEGIN;
	const uint64_t canary_end = DICT_END;
	uint32_t n = (uint32_t)ctx->active_count;

	// Worst case invariants guaranteed by the pre-allocation in InitThreadContext
	assert(ctx->nb_urls_dicts_size >= n);
	assert(ctx->nb_ips_dicts_size >= n);

	uint32_t* url_indices = ctx->nb_urls_dicts;
	uint32_t* ip_indices = ctx->nb_ips_dicts;

	uint32_t url_count = 0;
	uint32_t ip_count = 0;

	// 1. Collection and deduplication of references within the batch
	for (uint32_t i = 0; i < n; ++i) {
		SerializedEntry* entry = &ctx->active_buffer[i];

		// Filtering unique URL references
		if (entry->url_idx != UINT32_MAX && ctx->url_cache[entry->url_idx].used_in_shard) {
			url_indices[url_count++] = entry->url_idx;
			ctx->url_cache[entry->url_idx].used_in_shard = 0;
			LOG_DEBUG("Collecte URL : %d", entry->url_idx);
			LOG_DEBUG("Collecte URL : %s", ctx->url_cache[entry->url_idx].value);
		}

		// Filtering unique IP references
		if (entry->ip_idx != UINT32_MAX && ctx->ipv6_cache[entry->ip_idx].used_in_shard) {
			ip_indices[ip_count++] = entry->ip_idx;
			ctx->ipv6_cache[entry->ip_idx].used_in_shard = 0;
			LOG_DEBUG("Collecte IP : %d", entry->ip_idx);
			LOG_DEBUG("Collecte IP : %s", ctx->ipv6_cache[entry->ip_idx].value);
		}
	}

	BYTE* buf = (BYTE*)ctx->dictbuf;
	BYTE* p = buf;

	// 2. Direct serialization (Zero allocation, Zero realloc, Single pass)

	// Canary start	
	memcpy(p, &canary_begin, sizeof(canary_begin));
	p += sizeof(canary_begin);
	LOG_DEBUG("canary OK");

	// Section URLs
	memcpy(p, &url_count, sizeof(url_count));
	p += sizeof(url_count);
	LOG_DEBUG("nb_url %d", url_count);

	for (uint32_t i = 0; i < url_count; i++) {
		uint32_t id = url_indices[i];
		uint16_t url_len = ctx->url_cache[id].value_len;

		memcpy(p, &id, sizeof(id));
		p += sizeof(id);
		memcpy(p, &url_len, sizeof(url_len));
		p += sizeof(url_len);

		memcpy(p, ctx->url_cache[id].value, url_len);
		p += url_len;
		LOG_DEBUG("url len %d, url id %u", url_len, id);
	}

	// Section IPs
	memcpy(p, &ip_count, sizeof(ip_count));
	p += sizeof(ip_count);
	LOG_DEBUG("nb_ip %d", ip_count);

	for (uint32_t i = 0; i < ip_count; i++) {
		uint32_t id = ip_indices[i];
		uint16_t ip_len = ctx->ipv6_cache[id].value_len;

		memcpy(p, &id, sizeof(id));
		p += sizeof(id);
		memcpy(p, &ip_len, sizeof(ip_len));
		p += sizeof(ip_len);

		memcpy(p, ctx->ipv6_cache[id].value, ip_len);
		p += ip_len;
		LOG_DEBUG("ip len %d, ip id %u", ip_len, id);
	}

	// Canary end
	memcpy(p, &canary_end, sizeof(canary_end));
	p += sizeof(canary_end);
	LOG_DEBUG("End canary OK");

	// 3. Instant calculation of the generated binary size
	size_t actual_size = (size_t)(p - buf);

	// Security check against any overflow of the pre-allocated capacity
	assert(actual_size <= ctx->dictbuf_size);
	if (actual_size > ctx->dictbuf_size) {
		char msg[256];
		snprintf(msg, sizeof(msg), "DICSIZEERROR => dictbuf overflow %zu > %zu\n", actual_size, ctx->dictbuf_size);
		WriteError(herror, msg);
		*out_size = 0;
		return NULL;
	}

	*out_size = actual_size;
	return buf;
}


/*==============================================================================
 * LogFlush - Compresses and flushes the thread's active buffer to disk.
 *============================================================================*/
uint8_t PulpFlush(void* ctx_override) {
	LOG_DEBUG("LogFlush begin for context %lu", tls_index);

	// 1. Choice of context: explicit override or default TLS
	ThreadContext* ctx = ctx_override
		? (ThreadContext*)ctx_override
		: (ThreadContext*)TlsGetValue(tls_index);

	if (!ctx || ctx->active_count == 0) {
		LOG_DEBUG("LogFlush nothing to flush");
		return RTN_OK;
	}

	// 1.1 Creation of the file or rotation if necessary
	ctx->flush_count++;
	if ((ctx->file_handle == INVALID_HANDLE_VALUE) || (ctx->flush_count & (flush_per_file - 1)) == 0) {
		WriteTask t = {
			.ctx = ctx,
			.data = NULL,
			.file_handle = INVALID_HANDLE_VALUE,
			.size = 0,
			.taskType = TASK_CREATE_OR_ROTATE
		};
		WritePool_Enqueue(t);
	}

	LOG_DEBUG("LogFlush file handle : %p", ctx->file_handle);
	LOG_DEBUG("TLS GET CTX + ACTIVE COUNT %p %zu", ctx, ctx->active_count);

	// 2. Copy serialized entries
	size_t data_size = ctx->active_count * sizeof(SerializedEntry);
	memcpy(ctx->databuf, ctx->active_buffer, data_size);

	// 3. Build the dictionary
	size_t dict_size = 0;
	BYTE* dict_buf = BuildDictionaryInMemory(ctx, &dict_size);
	if (!dict_buf || dict_size == 0) {
		// Empty or invalid dictionary: clean cancellation
		ctx->active_count = 0;
		STATS_INC64(compression_failure_total);
		return RTN_INTERNAL_FAILURE_1;
	}

	 // 3.1 Wait for the previous compression to complete before overwriting pending_buffer
	 // This code is there for automated analysis conformity purposes, but in practice, the compression is always completed before the next flush.
	if (ctx->pending_compression > 0) {
		EnterCriticalSection(&ctx->compression_cs);
		while (ctx->pending_compression > 0) {
			SleepConditionVariableCS(&ctx->pc_complete, &ctx->compression_cs, INFINITE);
		}
		LeaveCriticalSection(&ctx->compression_cs);
	}

	// 4. Concatenation into pending_buffer [Data + Dictionary]
	ctx->pending_size = data_size + dict_size;
	memcpy(ctx->pending_buffer, ctx->databuf, data_size);
	memcpy((BYTE*)ctx->pending_buffer + data_size, dict_buf, dict_size);

	LOG_DEBUG("data_size = %zu, dict_size = %zu, pending_size = %zu",
		data_size, dict_size, ctx->pending_size);

	// 5. Launch asynchronous compression
	InterlockedIncrement(&ctx->pending_compression);
	PTP_WORK work = CreateThreadpoolWork(CompressionTask, ctx, &g_envCompress);
	if (work == NULL) {
		ctx->pending_size = 0;
		InterlockedDecrement(&ctx->pending_compression);
		ctx->active_count = 0; // Mandatory release of the active buffer
		STATS_INC64(compression_failure_total);
		return RTN_INTERNAL_FAILURE_4;
	}

	SubmitThreadpoolWork(work);
	CloseThreadpoolWork(work);

	// 6. Reset the active buffer for the next batch
	ctx->active_count = 0;

	LOG_DEBUG("LogFlush end");
	return RTN_OK;
}

void PulpShutdown(void) {
	LOG_DEBUG("LogShutdown begin");
	// 0) Atomically detach the global list
	//    retrieve the head and set global_context_list to NULL
	EnterCriticalSection(&context_list_lock);
	ContextList* head = global_context_list;
	global_context_list = NULL;
	LeaveCriticalSection(&context_list_lock);


	// 1) Flush all contexts
	for (ContextList* node = head; node; node = node->next) {
		ThreadContext* ctx = node->ctx;
		if (ctx) {
			STATS_INC64(batch_flushed_total);
			PulpFlush(ctx);
		}
		Sleep(5);
	}

	// 2) Wait for the completion of compressions + writes for each context
	for (ContextList* node = head; node; node = node->next) {
		ThreadContext* ctx = node->ctx;
		if (!ctx) continue;
		// A) End of compressions
		EnterCriticalSection(&ctx->compression_cs);
		while (ctx->pending_compression > 0) {
			SleepConditionVariableCS(&ctx->pc_complete, &ctx->compression_cs, INFINITE);
		}
		LeaveCriticalSection(&ctx->compression_cs);
		Sleep(5);
		// B) End of writes
		AcquireSRWLockExclusive(&ctx->file_lock);
		// Force flush to disk
		BOOL v = FlushFileBuffers(ctx->file_handle);
		if (v == FALSE) {
			char msg[256];
			snprintf(msg, sizeof(msg), "ShutDown => %d file %p flush failed / %s / %lu\n",
				__LINE__, ctx->file_handle, use_backup_path ? backup_path : base_path, GetLastError());
			WriteError(herror, msg);
		}
		while (ctx->pending_write > 0) {
			SleepConditionVariableSRW(&ctx->pw_complete, &ctx->file_lock, INFINITE, 0);
		}
		ReleaseSRWLockExclusive(&ctx->file_lock);
		Sleep(5);
	}



#if PULP_STAT_ACTIVE
	LOG_STAT("cache_hit resource L1 : %I64d", g_stats.L1_url_cache_hit);
	LOG_STAT("cache_hit resource L1 %% : %.2f", (float)(g_stats.L1_url_cache_hit * 100) / (float)(g_stats.log_processed_total));
	LOG_STAT("cache_hit resource L2 : %I64d", g_stats.L2_url_cache_hit);
	LOG_STAT("cache_hit resource L2 %% : %.2f", (float)(g_stats.L2_url_cache_hit * 100) / (float)(g_stats.log_processed_total));
	LOG_STAT("cache_hit resource L3 : %I64d", g_stats.L3_url_cache_hit);
	LOG_STAT("cache_hit resource L3 %% : %.2f", (float)(g_stats.L3_url_cache_hit * 100) / (float)(g_stats.log_processed_total));
	
	LOG_STAT("cache_hit endpoint L1 : %I64d", g_stats.L1_ip_cache_hit);
	LOG_STAT("cache_hit endpoint L1 %% : %.2f", (float)(g_stats.L1_ip_cache_hit * 100) / (float)(g_stats.log_processed_total));
	LOG_STAT("cache_hit endpoint L2 : %I64d", g_stats.L2_ip_cache_hit);
	LOG_STAT("cache_hit endpoint L2 %% : %.2f", (float)(g_stats.L2_ip_cache_hit * 100) / (float)(g_stats.log_processed_total));
	LOG_STAT("cache_hit endpoint L3 : %I64d", g_stats.L3_ip_cache_hit);
	LOG_STAT("cache_hit endpoint L3 %% : %.2f", (float)(g_stats.L3_ip_cache_hit * 100) / (float)(g_stats.log_processed_total));

	LOG_STAT("resource_cache_probes_total : %llu", g_stats.url_cache_probes_total);
	LOG_STAT("endpoint_cache_probes_total : %llu", g_stats.ip_cache_probes_total);
	
	LOG_STAT("resource cache_insert_to_step_ratio : %.2f", 
		(float)((g_stats.L1_url_cache_hit + g_stats.L2_url_cache_hit + g_stats.L3_url_cache_hit) / (float)g_stats.url_cache_probes_total));
	LOG_STAT("endpoint cache_insert_to_step_ratio : %.2f",
		(float)((g_stats.L1_ip_cache_hit + g_stats.L2_ip_cache_hit + g_stats.L3_ip_cache_hit) / (float)g_stats.ip_cache_probes_total));

	LOG_STAT("resource cache_probes_depth_max : %llu", g_stats.url_cache_probes_max);
	LOG_STAT("endpoint cache_probes_depth_max : %llu", g_stats.ip_cache_probes_max);

	LOG_STAT("resource_cache_fullprobescan_total : %llu", g_stats.url_cache_fullprobescan_total);
	LOG_STAT("endpoint_cache_fullprobescan_total : %llu", g_stats.ip_cache_fullprobescan_total);

	LOG_STAT("log_processed_total : %llu", g_stats.log_processed_total);
	LOG_STAT("batch_flushed_total : %llu", g_stats.batch_flushed_total);
	LOG_STAT("batch_compressed_total : %llu", g_stats.batch_compressed_total);
	LOG_STAT("batch_written_total : %llu", g_stats.batch_written_total);
	LOG_STAT("writer_waitfile_max : %llu", g_stats.writer_waitfile_max);
	LOG_STAT("backpressure_count : %llu", g_stats.backpressure_count);
	LOG_STAT("compression_lz4_ratio_avg : %.2f", (float)(g_stats.compression_lz4_ratio_avg / (g_stats.batch_compressed_total * 10000.0f)));
	LOG_STAT("compression_failure_total : %llu", g_stats.compression_failure_total);
	LOG_STAT("lost_logs_total : %llu", g_stats.lost_logs_total);
	LOG_STAT("log_rotation : %llu", g_stats.log_rotation);
	LOG_STAT("log_refused_total : %llu", g_stats.log_refused_total);
	LOG_STAT("rotation_resync_total : %llu", g_stats.rotation_resync_total);
#endif
	Sleep(5);

	// 3) Stop and join the write pool and compression workers
	WritePool_Shutdown();

	// 4) Close handles, free buffers and contexts, free list nodes
	for (ContextList* node = head; node; ) {
		ContextList* next = node->next;
		ThreadContext* ctx = node->ctx;
		if (ctx) {
			// 4.1) If there are pending operations
			while (InterlockedCompareExchange(&ctx->pending_compression, 0, 0) > 0)
				Sleep(25);
			while (InterlockedCompareExchange(&ctx->pending_write, 0, 0) > 0)
				Sleep(25);
			// 4.2) Close the file handle
			AcquireSRWLockExclusive(&ctx->file_lock);
			if (ctx->file_handle && ctx->file_handle != INVALID_HANDLE_VALUE) {
				CloseHandle(ctx->file_handle);
			}
			ReleaseSRWLockExclusive(&ctx->file_lock);
			// 4.3) Free buffers
			if (ctx->pending_buffer) {
				_aligned_free(ctx->pending_buffer);
			}
			if (ctx->active_buffer) {
				_aligned_free(ctx->active_buffer);
			}
			if (ctx->url_cache) {
				_aligned_free(ctx->url_cache);
			}
			if (ctx->ipv6_cache) { 
				_aligned_free(ctx->ipv6_cache); 
			}
			if (ctx->databuf) {
				_aligned_free(ctx->databuf);
			}
			if (ctx->dictbuf) {
				_aligned_free(ctx->dictbuf);
			}
			if (ctx->nb_ips_dicts) {
				_aligned_free(ctx->nb_ips_dicts);
			}
			if (ctx->nb_urls_dicts) {
				_aligned_free(ctx->nb_urls_dicts);
			}

			DeleteCriticalSection(&ctx->compression_cs);
			_aligned_free(ctx);
		}
		// 4.4) Free the list node
		free(node);
		node = next;
	}

	// 5) Free the TLS
	if (tls_index != TLS_OUT_OF_INDEXES) {
		TlsFree(tls_index);
		tls_index = TLS_OUT_OF_INDEXES;
	}

	// 6) Free the compression pool
	DestroyThreadpoolEnvironment(&g_envCompress);
	if (g_threadPool) {
		CloseThreadpool(g_threadPool);
		g_threadPool = NULL;
	}

	LOG_DEBUG("LogShutdown end");
}