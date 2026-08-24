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


// write_pool.c
#include "pulp_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


typedef struct {
	HANDLE* pending_handles_tab;
	uint32_t pending_close_index;
	uint32_t hclosing;
} PendingHandles;

PendingHandles global_pending_handles; // Handles to be closed later

WritePool global_write_pool;
WriteTask* rotation_queue;
uint16_t rotation_index;
HANDLE rotation_thread;                    // file rotation thread
HANDLE file_rotation_semaphore;            // Synchronization for reenqueue function


char* GetShardPath(LONG64 file_num) {
	static __declspec(thread) char log_path[MAX_PATH];  // TLS to avoid conflicts
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	ULARGE_INTEGER uli;
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	_snprintf_s(log_path, MAX_PATH, _TRUNCATE, "%s\\shard_%I64d_%I64d.bin", use_backup_path ? backup_path : base_path, uli.QuadPart , file_num);
	LOG_DEBUG("GetShardPath : %s", log_path);
	return log_path;
}

HANDLE CreateNewShard() {
RETRY_AFTER_SWAP :
	const char* log_path = GetShardPath(InterlockedIncrement64(&global_file_counter));
	LOG_DEBUG("CreatenewShard begin");

	HANDLE f = CreateFileA(
		log_path,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
		NULL);
	if (f == INVALID_HANDLE_VALUE) {
		DWORD last_error = GetLastError();
		char msg[256];
		snprintf(msg, sizeof(msg), "CSERROR => file %d / %s / %lu\n", __LINE__, log_path, last_error);
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		if (last_error == ERROR_DISK_FULL || last_error == ERROR_PATH_NOT_FOUND ||
			last_error == ERROR_ACCESS_DENIED) {
			EnterCriticalSection(&global_write_pool.pathswap);
			uint8_t retry_after_swap = 0;
			if (InterlockedCompareExchange(&use_backup_path, 0, 0) == 0) {
				InterlockedExchange(&use_backup_path, 1);
				snprintf(msg, sizeof(msg), "CSERROR => %d switched to backup path because %lu / %s \n",
					__LINE__, last_error, InterlockedCompareExchange(&use_backup_path, 0, 0) ? backup_path : base_path);
				WriteError(herror, msg);
				retry_after_swap = 1;
			}
			LeaveCriticalSection(&global_write_pool.pathswap);
			if(retry_after_swap)
				goto RETRY_AFTER_SWAP;
		}
	}
	return f;
}

HANDLE CreateErrorFile(char* errorpath) {

	char local_error_path[MAX_PATH] = { 0 };
	_snprintf_s(local_error_path, MAX_PATH, _TRUNCATE, "%s\\_error_.txt", errorpath);

	HANDLE f = CreateFileA(
		local_error_path,
		FILE_APPEND_DATA,
		FILE_SHARE_READ,
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
		NULL);
	if (f == INVALID_HANDLE_VALUE) {
		char msg[256];
		snprintf(msg, sizeof(msg), "CEERROR => file %d / %s / %lu\n", __LINE__, error_path, GetLastError());
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
	}

	return f;
}

DWORD WINAPI WriteThread(LPVOID param) {
	LOG_DEBUG("WriteThread begin");
	for (;;) {
		WaitForSingleObject(global_write_pool.writetask_semaphore, INFINITE);

		WriteTask task = { 0 };
		bool has_task = false;
		bool task_was_write = false;

		EnterCriticalSection(&global_write_pool.queuelock);
		if (global_write_pool.head != global_write_pool.tail) {
			task = global_write_pool.tasks[global_write_pool.tail];
			global_write_pool.tail = (global_write_pool.tail + 1) & (global_write_pool.queue_size - 1);
			has_task = true;
		}
		else if (InterlockedCompareExchange(&global_write_pool.running, 0, 0) == 0) {
			// Complete drainage: the queue is empty AND the system is shutting down
			LeaveCriticalSection(&global_write_pool.queuelock);
			break;
		}
		LeaveCriticalSection(&global_write_pool.queuelock);

		if (!has_task)
			continue;

		switch (task.taskType) {
		case TASK_WRITE:
			task_was_write = true;
			{
				uint8_t tries = 0;
			RETRY:
				if (task.ctx && task.ctx->file_handle == INVALID_HANDLE_VALUE) {
					AcquireSRWLockShared(&task.ctx->file_lock);
					if (task.ctx->file_handle == INVALID_HANDLE_VALUE) {
						// Wait (lock released during wait, reacquired upon wake)
						SleepConditionVariableSRW(&task.ctx->file_ready_cv, &task.ctx->file_lock,
							500, CONDITION_VARIABLE_LOCKMODE_SHARED);
					}
					ReleaseSRWLockShared(&task.ctx->file_lock);

					// Double-check under exclusive lock before creating
					if (task.ctx->file_handle == INVALID_HANDLE_VALUE) {
						AcquireSRWLockExclusive(&task.ctx->file_lock);
						if (task.ctx->file_handle == INVALID_HANDLE_VALUE) {
							task.ctx->file_handle = CreateNewShard();
						}
						ReleaseSRWLockExclusive(&task.ctx->file_lock);
					}

					if (tries++ < 3)
						goto RETRY;
				}

				if (!task.ctx || task.ctx->file_handle == INVALID_HANDLE_VALUE) {
					STATS_INC64(lost_logs_total);
					char msg[256];
					snprintf(msg, sizeof(msg), "WTERROR => lost batch line %d / ctx=%p / handle=%p\n",
						__LINE__, task.ctx, task.ctx ? task.ctx->file_handle : NULL);
					LOG_ERROR("%s", msg);
					WriteError(herror, msg);
					break;
				}

				DWORD total = 0;
				tries = 0;
				for (;;) {
					DWORD written = 0;
					AcquireSRWLockExclusive(&task.ctx->file_lock);
					BOOL ok = WriteFile(task.ctx->file_handle,
						(BYTE*)task.data + total,
						(DWORD)(task.size - total),
						&written,
						NULL);
					ReleaseSRWLockExclusive(&task.ctx->file_lock);

					if (!ok) {
						DWORD err = GetLastError();
						if (++tries < SPIN_LIMIT && (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_WRITE_PROTECT || err == ERROR_LOCK_VIOLATION)) {
							Sleep(1 + tries);
							continue;
						}
						// Fallback to backup path
						if ((err == ERROR_DISK_FULL || err == ERROR_PATH_NOT_FOUND || err == ERROR_ACCESS_DENIED) && backup_path[0]) {
							DWORD error_code = GetLastError();
							EnterCriticalSection(&global_write_pool.pathswap);
							if (InterlockedCompareExchange(&use_backup_path, 0, 0) == 0) {
								InterlockedExchange(&use_backup_path, 1);
								char msg[256];
								snprintf(msg, sizeof(msg), "WTERROR => %d swapped to backup because %lu / %s / TID=%lu\n",
									__LINE__, error_code,
									InterlockedCompareExchange(&use_backup_path, 0, 0) ? backup_path : base_path,
									GetCurrentThreadId());
								WriteError(herror, msg);
							}
							LeaveCriticalSection(&global_write_pool.pathswap);

							AcquireSRWLockExclusive(&task.ctx->file_lock);
							if (task.ctx->file_handle != INVALID_HANDLE_VALUE) {
								CloseHandle(task.ctx->file_handle);
							}
							task.ctx->file_handle = CreateNewShard();
							ReleaseSRWLockExclusive(&task.ctx->file_lock);
							WakeAllConditionVariable(&task.ctx->file_ready_cv);

							tries = 0;
							total = 0;
							continue;
						}

						char msg[256];
						snprintf(msg, sizeof(msg), "WAERROR => write failed on primary & backup (line %d / err %lu)\n", __LINE__, err);
						WriteError(herror, msg);
						STATS_INC64(lost_logs_total);
						break;
					}

					total += written;
					if (total >= task.size) {
						STATS_INC64(batch_written_total);
						break;
					}

					if (++tries > SPIN_LIMIT) {
						WriteError(herror, "WAERROR => retry limit reached, partial write\n");
						STATS_INC64(lost_logs_total);
						break;
					}
				}
			}
			break;

		default:
		{
			char msg[256];
			snprintf(msg, sizeof(msg), "WTERROR => unknown task type %d (line %d)\n", task.taskType, __LINE__);
			LOG_ERROR("%s", msg);
			WriteError(herror, msg);
		}
		break;
		}

		// Unified atomic decrement and memory release
		if (task_was_write) {
			if (task.ctx) {
				if (InterlockedDecrement(&task.ctx->pending_write) == 0) {
					WakeAllConditionVariable(&task.ctx->pw_complete);
				}
			}
			if (task.data) {
				free(task.data);
			}
		}
	}
	return 0;
}

/* Create the new files when needed */
static DWORD WINAPI RotationThread(LPVOID param) {
	WriteTask* local_task = (WriteTask*)calloc(g_max_rotation_queue, sizeof(WriteTask));

	if (local_task == NULL || global_pending_handles.pending_handles_tab == NULL) {
		char msg[256];
		snprintf(msg, sizeof(msg), "RERROR => malloc failed line %d (%llu bytes)\n", __LINE__, g_max_rotation_queue * sizeof(WriteTask));
		WriteError(herror, msg);
		if (local_task)
			free(local_task);
		return (DWORD)-1;
	}

	for (;;) {
		// Timeout of 100 ms to eliminate the active loop at 100% CPU
		DWORD waitResult = WaitForSingleObject(file_rotation_semaphore, 100);

		EnterCriticalSection(&global_write_pool.rotationlock);
		uint16_t current_task_index = rotation_index;

		// Clean shutdown: queue is empty AND pool is shutting down
		if (current_task_index == 0 && InterlockedCompareExchange(&global_write_pool.running, 0, 0) == 0) {
			LeaveCriticalSection(&global_write_pool.rotationlock);
			break;
		}

		for (uint16_t i = 0; i < current_task_index; i++) {
			local_task[i] = rotation_queue[i];
			memset(&rotation_queue[i], 0, sizeof(WriteTask));
		}
		rotation_index = 0;
		LeaveCriticalSection(&global_write_pool.rotationlock);

		for (uint16_t i = 0; i < current_task_index; i++) {
			if (local_task[i].ctx == NULL) {
				char msg[256];
				snprintf(msg, sizeof(msg), "RERROR => line %d NULL context\n", __LINE__);
				WriteError(herror, msg);
				continue;
			}

			HANDLE f = CreateNewShard();
			if (f == INVALID_HANDLE_VALUE) {
				char msg[256];
				snprintf(msg, sizeof(msg), "RERROR => file creation failed line %d (%s, err=%lu)\n",
					__LINE__, use_backup_path ? backup_path : base_path, GetLastError());
				WriteError(herror, msg);
				continue;
			}

			AcquireSRWLockExclusive(&local_task[i].ctx->file_lock);
			while (InterlockedCompareExchange(&local_task[i].ctx->pending_write, 0, 0) > 0) {
				SleepConditionVariableSRW(
					&local_task[i].ctx->pw_complete,
					&local_task[i].ctx->file_lock,
					INFINITE,
					0);
			}

			if (local_task[i].ctx->file_handle != INVALID_HANDLE_VALUE) {
				BOOL v = FlushFileBuffers(local_task[i].ctx->file_handle);
				if (v == FALSE) {
					char msg[256];
					snprintf(msg, sizeof(msg), "RERROR => line %d file %p flush failed (%s, err=%lu)\n",
						__LINE__, local_task[i].ctx->file_handle,
						use_backup_path ? backup_path : base_path, GetLastError());
					WriteError(herror, msg);
				}
			}

			HANDLE oldhandle = local_task[i].ctx->file_handle;
			local_task[i].ctx->file_handle = f;
			LOG_DEBUG("CREATE NEW SHARD : updated handle in ctx => %p", local_task[i].ctx->file_handle);
			ReleaseSRWLockExclusive(&local_task[i].ctx->file_lock);
			WakeAllConditionVariable(&local_task[i].ctx->file_ready_cv);

			if (oldhandle != INVALID_HANDLE_VALUE) {
				if (global_pending_handles.pending_close_index < g_max_pending_handles) {
					global_pending_handles.pending_handles_tab[global_pending_handles.pending_close_index++] = oldhandle;
				}
				else {
					LOG_TRACE("Closing handle");
					CloseHandle(global_pending_handles.pending_handles_tab[global_pending_handles.hclosing]);
					global_pending_handles.pending_handles_tab[global_pending_handles.hclosing] = oldhandle;
					global_pending_handles.hclosing = (global_pending_handles.hclosing + 1) & (g_max_pending_handles - 1);
				}
			}
			STATS_INC64(log_rotation);
		}
	}

	free(local_task);
	return 0;
}

BOOL WritePool_Init() {
	// Allocation of arrays
	global_write_pool.tasks = (WriteTask*)malloc(g_write_queue_size * sizeof(WriteTask));
	global_write_pool.threads = (HANDLE*)malloc(g_write_pool_thread_count * sizeof(HANDLE));
	rotation_queue = (WriteTask*)malloc(g_max_rotation_queue * sizeof(WriteTask));

	if (!global_write_pool.tasks || !global_write_pool.threads || !rotation_queue) {
		char msg[256];
		snprintf(msg, sizeof(msg), "WPERROR => file %d / %llu / %llu / %llu\n",
			__LINE__, 
			g_write_queue_size * sizeof(WriteTask), 
			g_write_pool_thread_count * sizeof(HANDLE), 
			g_max_rotation_queue * sizeof(WriteTask));
		WriteError(herror, msg);
		return FALSE;
	}

	global_write_pool.writetask_semaphore = CreateSemaphore(NULL, 0, g_write_queue_size, NULL);
	file_rotation_semaphore = CreateSemaphore(NULL, 0, g_max_rotation_queue, NULL);
	if (global_write_pool.writetask_semaphore == NULL || file_rotation_semaphore == NULL) {
		char msg[256];
		snprintf(msg, sizeof(msg), "WPIERROR => file %d / %lu\n", __LINE__, GetLastError());
		WriteError(herror, msg);
		return FALSE;
	}

	global_write_pool.head = 0;
	global_write_pool.tail = 0;
	global_pending_handles.hclosing = 0;
	global_pending_handles.pending_close_index = 0;
	global_pending_handles.pending_handles_tab = (HANDLE*)calloc(g_max_pending_handles, sizeof(HANDLE));
	if (global_pending_handles.pending_handles_tab == NULL) {
		char msg[256];
		snprintf(msg, sizeof(msg), "WPIERROR => malloc %d / %llu\n", __LINE__, g_max_pending_handles * sizeof(HANDLE));
		WriteError(herror, msg);
		return FALSE;
	}
	rotation_index = 0;
	memset(rotation_queue, 0, sizeof(WriteTask) * g_max_rotation_queue);
	memset(global_pending_handles.pending_handles_tab, 0, sizeof(HANDLE) * g_max_pending_handles);

	LOG_DEBUG("WritePool Init begin");
	global_write_pool.running = TRUE;
	rotation_thread = CreateThread(
		NULL, 0, (LPTHREAD_START_ROUTINE)RotationThread, rotation_queue, 0, NULL);
	for (uint32_t i = 0; i < g_write_pool_thread_count; i++) {
		global_write_pool.threads[i] = CreateThread(
			NULL, 0, (LPTHREAD_START_ROUTINE)WriteThread, NULL, 0, NULL
		);
		if (global_write_pool.threads[i] == NULL || rotation_thread == NULL) {
			// Rollback
			for (uint32_t j = 0; j < i; j++) {
				CloseHandle(global_write_pool.threads[j]);
			}
			if(global_write_pool.tasks)
				free(global_write_pool.tasks);
			if(global_write_pool.threads)
				free(global_write_pool.threads);
			if(rotation_queue)
				free(rotation_queue);
			if (rotation_thread)
				CloseHandle(rotation_thread);
			if(global_write_pool.writetask_semaphore)
				CloseHandle(global_write_pool.writetask_semaphore);
			if(file_rotation_semaphore)
				CloseHandle(file_rotation_semaphore);
			return FALSE;
		}
	}

	LOG_DEBUG("WritePool Init end");
	return TRUE;
}

void WritePool_Shutdown() {
	// 1) Signal shutdown
	InterlockedAnd(&global_write_pool.running, 0);

	// 2) Calculate the number of pending tasks
	EnterCriticalSection(&global_write_pool.queuelock);
	int pendingTasks = (global_write_pool.head - global_write_pool.tail + g_write_queue_size) & (g_write_queue_size - 1);
	int rotationTask = rotation_index;
	LeaveCriticalSection(&global_write_pool.queuelock);

	// 3) Wake up enough threads to drain the queues
	int wakeCount = pendingTasks + g_write_pool_thread_count + rotationTask;
	for (int i = 0; i < wakeCount; i++) {
		ReleaseSemaphore(global_write_pool.writetask_semaphore, 1, NULL);
		ReleaseSemaphore(file_rotation_semaphore, 1, NULL);
	}

	// 4) Wait for threads to finish
	WaitForMultipleObjects(g_write_pool_thread_count, global_write_pool.threads, TRUE, 10000);
	WaitForSingleObject(rotation_thread, 10000);

	// 5) Close thread handles
	for (uint32_t i = 0; i < g_write_pool_thread_count; i++) {
		CloseHandle(global_write_pool.threads[i]);
	}
	CloseHandle(rotation_thread);

	// 6) Close all pending handles
	EnterCriticalSection(&global_write_pool.filelock);
	if (global_pending_handles.pending_handles_tab != NULL && global_pending_handles.pending_close_index != 0) {
		for (uint32_t i = 0; i < global_pending_handles.pending_close_index; i++) {
			if (global_pending_handles.pending_handles_tab[i] != NULL &&
				global_pending_handles.pending_handles_tab[i] != INVALID_HANDLE_VALUE) {
				CloseHandle(global_pending_handles.pending_handles_tab[i]);
			}
		}
	}
	global_pending_handles.pending_close_index = 0;
	LeaveCriticalSection(&global_write_pool.filelock);

	// 7) Cleanup resources and deallocation
	CloseHandle(global_write_pool.writetask_semaphore);
	CloseHandle(file_rotation_semaphore);
	DeleteCriticalSection(&global_write_pool.filelock);
	DeleteCriticalSection(&global_write_pool.queuelock);
	DeleteCriticalSection(&global_write_pool.rotationlock);
	DeleteCriticalSection(&global_write_pool.errorlock);
	DeleteCriticalSection(&global_write_pool.pathswap);

	free(global_write_pool.tasks);
	free(global_write_pool.threads);
	free(rotation_queue);

	if (global_pending_handles.pending_handles_tab) {
		free(global_pending_handles.pending_handles_tab);
		global_pending_handles.pending_handles_tab = NULL;
	}

	LOG_DEBUG("WritePool_Shutdown end");
}

/*
   WritePool_Enqueue : Queue for write tasks
*/
void WritePool_Enqueue(WriteTask task) {
	LOG_DEBUG("WritePool enqueue begin");
	if (!task.ctx) {
		char msg[256];
		snprintf(msg, sizeof(msg), "WPEERROR => context %d / %p\n", __LINE__, NULL);
		LOG_ERROR("%s", msg);
		WriteError(herror, msg);
		if (task.data) free(task.data);
		return;
	}

	// 1. WRITE TASKS
	if (task.taskType == TASK_WRITE) {
		uint8_t retry_count = 0;
		while (retry_count < SPIN_LIMIT) {
			EnterCriticalSection(&global_write_pool.queuelock);
			LONG next = (global_write_pool.head + 1) & (global_write_pool.queue_size - 1);
			if (next != global_write_pool.tail) {
				// Increment only if the task is actually enqueued
				InterlockedIncrement(&task.ctx->pending_write);
				STATS_MAX(writer_waitfile_max, ((global_write_pool.head - global_write_pool.tail) & (g_write_queue_size - 1)));
				global_write_pool.tasks[global_write_pool.head] = task;
				global_write_pool.head = next;
				LeaveCriticalSection(&global_write_pool.queuelock);

				ReleaseSemaphore(global_write_pool.writetask_semaphore, 1, NULL);
				return;
			}
			LeaveCriticalSection(&global_write_pool.queuelock);

			Sleep(1 + retry_count);
			retry_count++;
		}

		// File saturated -> Controlled shedding
		// pending_write was not incremented: simple memory release and log.
		STATS_INC64(lost_logs_total);
		char msg[256];
		snprintf(msg, sizeof(msg), "WPEWARN => write task dropped after %d retries (size=%zu)\n", retry_count, task.size);
		WriteError(herror, msg);

		if (task.data) {
			free(task.data);
			task.data = NULL;
		}
		return;
	}

	// 2. ROTATION TASKS
	if (task.taskType == TASK_CREATE_OR_ROTATE) {
		uint8_t retry_count = 0;
		while (retry_count < SPIN_LIMIT) {
			EnterCriticalSection(&global_write_pool.rotationlock);
			if (rotation_index < g_max_rotation_queue) {
				rotation_queue[rotation_index++] = task;
				LeaveCriticalSection(&global_write_pool.rotationlock);
				ReleaseSemaphore(file_rotation_semaphore, 1, NULL);
				return;
			}
			LeaveCriticalSection(&global_write_pool.rotationlock);

			Sleep(1 + retry_count);
			retry_count++;
		}

		LOG_ERROR("ROTATION FAILED");
		WriteError(herror, "WPEWARN => file rotation dropped\n");
		return;
	}

	// 3. UNKNOWN TASK TYPE
	STATS_INC64(lost_logs_total);
	WriteError(herror, "WPEERROR => unknown task type\n");
	if (task.data) free(task.data);
}

void WriteError(HANDLE local_herror, char* msg) {
	
	if (local_herror == INVALID_HANDLE_VALUE) return;
	EnterCriticalSection(&global_write_pool.errorlock);
	// Get the current time
	time_t now = time(NULL);
	struct tm local;
	localtime_s(&local, &now);

	// Build the message with timestamp
	char buffer[1024];
	uint32_t len = snprintf(buffer, sizeof(buffer),
			"[%04d-%02d-%02d %02d:%02d:%02d] %s",
			local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
			local.tm_hour, local.tm_min, local.tm_sec,
			msg);

	// Write to the file
	uint32_t total = 0;
	uint32_t tries = 0;
	if (len > 0 && len < sizeof(buffer)) {
		for (;;) {
			DWORD written;
			WriteFile(local_herror, buffer, len, &written, NULL);
			total += written;
			if (total >= len) {
				break;
			}
			// Partial write
			if (tries++ >= SPIN_LIMIT)
				break;
		}
	}
	LeaveCriticalSection(&global_write_pool.errorlock);
}