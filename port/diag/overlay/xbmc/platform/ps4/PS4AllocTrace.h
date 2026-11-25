/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>

/* Diagnostic only: logs allocation failures and very large successful
 * allocations to /data/kodi-ps4/crash.log when /data/kodi-ps4/alloc-trace
 * (or PS4_ALLOC_TRACE=1) exists.  The operator new override itself is always
 * linked so the failing size/caller is available even when the log is off. */
void PS4AllocTraceWrite(std::size_t size, const void* caller);

/* Always logs a failed allocation, independently of alloc-trace: failures are
 * rare and the size/caller is the key evidence for the OOM crashes. */
void PS4AllocFailureWrite(std::size_t size, const void* caller);

/* Diagnostic memory map: when /data/kodi-ps4/mem-map (or PS4_MEM_MAP=1)
 * exists, track live allocations by immediate caller and append a snapshot
 * (live bytes + growth since the previous snapshot, top callers) to
 * /data/kodi-ps4/mem-map.log.  Called from the mem-canary phase markers. */
void PS4MemMapStart();
void PS4MemMapSnapshot(const char* tag);

/* Emergency flexible-heap reserve.  It is allocated once before PVR/Home
 * start and released by operator new on the first failed allocation, then
 * the allocation is retried.  The malloc part is split into 64 KiB chunks
 * (below the musl mmap threshold) so freeing it feeds the small sbrk heap;
 * the kernel part is unmapped.  Configurable with PS4_MEMORY_RESERVE_MB or
 * /data/kodi-ps4/memory-reserve-mb (16..128 MB). */
void PS4MemoryReserveInit();
bool PS4MemoryReserveRelease();
/* Returns the malloc'd chunks of the emergency reserve to the libc heap. */
bool PS4HeapReserveRelease();
