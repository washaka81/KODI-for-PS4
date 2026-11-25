/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS4AllocTrace.h"

#include "utils/MemUtils.h"

#if defined(__ORBIS__)
#include <orbis/libkernel.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <new>
#include <thread>
#include <unistd.h>

namespace
{
/* A successful allocation this large is worth recording: it catches a
 * corrupt size calculation that overcommit would otherwise hide.  The
 * FOSDEM/list crash happens with decoded images well below 64 MB, so the
 * diagnostic threshold is 16 MB (a 4K RGBA image is ~33 MB, 4K RG8 is
 * ~16.6 MB; the 4->2 byte translation must stay visible). */
constexpr std::size_t BIG_ALLOCATION_THRESHOLD = 16 * 1024 * 1024;
/* Do not burn the emergency reserve on a speculative, genuinely large
 * allocation.  The fatal Home/PVR allocations are a few hundred bytes; the
 * reserve is for those and for medium control/graphics buffers. */
constexpr std::size_t RESERVE_RELEASE_MAX = 4 * 1024 * 1024;

/* Fixed-size live-allocation table.  It is deliberately not a std::map:
 * this code runs inside operator new/delete, including before main().
 * 2^19 slots keeps the load low for the ~120k live allocations seen during
 * Kodi startup; the probe limit below guarantees that a full or pathological
 * table cannot turn every allocation into a 100 ms scan (the first
 * 0.1.1ab build filled 2^17 slots and stalled Home for 92 s). */
constexpr std::size_t MAP_SLOTS = 1u << 19;
constexpr std::size_t MAP_SHARDS = 64;
constexpr std::size_t SHARD_SLOTS = MAP_SLOTS / MAP_SHARDS;
/* 64 probes saturated the table in the 395-channel run (full=69k).  A larger
 * probe window keeps the live-byte snapshot meaningful at the cost of a
 * slower diagnostic pass; mem-map is opt-in. */
constexpr std::size_t MAX_PROBES = 512;
constexpr std::uint64_t HEAP_RESERVE_MAGIC = 0x5053344852455345ULL; /* PS4HRESE */
constexpr std::uint64_t KERNEL_RESERVE_MAGIC = 0x455345524B345350ULL; /* PS4KRESE */
constexpr std::size_t MAX_CALLERS = 32;
constexpr std::uintptr_t TOMBSTONE = 1;
constexpr std::size_t MAP_LOG_LIMIT = 4u * 1024u * 1024u;

struct AllocEntry
{
  void* ptr;
  std::size_t size;
  const void* caller;
};

AllocEntry g_entries[MAP_SLOTS];
/* One lock per shard: allocation/free happen on the render, PVR, curl and
 * worker threads, and a single global spinlock turned the map into a
 * contention point during startup. */
std::atomic<bool> g_shardLocks[MAP_SHARDS] = {};
std::atomic_flag g_logLock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_mapStarted{false};
std::atomic<std::size_t> g_untrackedFrees{0};
std::atomic<std::size_t> g_full{0};
std::atomic<std::size_t> g_lastLive{0};
std::size_t g_logBytes;

/* Emergency reserve: one block allocated from the flexible heap before
 * PVR/Home, freed by operator new on the first failure.  std::atomic<void*>
 * is lock-free on x86-64, so the failure path does not allocate or lock. */
std::atomic<void*> g_memoryReserve{nullptr};
std::atomic<std::size_t> g_memoryReserveSize{0};
/* Separate malloc'd reserve: operator new needs free chunks in the libc
 * heap; a kernel flexible mapping (g_memoryReserve) is only useful for
 * pthread_create/mmap.  A single 32 MB malloc() is usually served by mmap
 * and freeing it does not add chunks to the small sbrk heap, so the reserve
 * is split into 64 KiB chunks below musl's mmap threshold.  They are
 * allocated consecutively before PVR/Home and freed in reverse order. */
constexpr std::size_t HEAP_RESERVE_CHUNK = 64 * 1024;
constexpr std::size_t MAX_HEAP_RESERVE_CHUNKS = 2048;
std::atomic<bool> g_heapReserveActive{false};
std::atomic<std::size_t> g_heapReserveCount{0};
std::atomic<std::size_t> g_heapReserveSize{0};
void* g_heapReserveChunks[MAX_HEAP_RESERVE_CHUNKS];

constexpr const char* MAP_LOG = "/data/kodi-ps4/mem-map.log";
constexpr const char* MAP_OLD_LOG = "/data/kodi-ps4/mem-map.old.log";
constexpr const char* RESERVE_LOG = "/data/kodi-ps4/memory-reserve.log";

std::size_t MemoryReserveBytes()
{
  int megabytes = 64;
  if (const char* env = std::getenv("PS4_MEMORY_RESERVE_MB"))
  {
    const int value = std::atoi(env);
    if (value >= 16 && value <= 128)
      megabytes = value;
  }
  else if (FILE* file = std::fopen("/data/kodi-ps4/memory-reserve-mb", "r"))
  {
    int value = 0;
    if (std::fscanf(file, "%d", &value) == 1 && value >= 16 && value <= 128)
      megabytes = value;
    std::fclose(file);
  }
  return static_cast<std::size_t>(megabytes) * 1024ULL * 1024ULL;
}

void WriteReserveLog(const char* tag)
{
  const int fd = open(RESERVE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return;

  const unsigned long long flex = static_cast<unsigned long long>(
      KODI::MEMORY::GetAvailableFlexibleMemorySize() / 1024ULL);
  char line[160];
  const int length = std::snprintf(line, sizeof(line), "PS4RESERVE tag=%s flex=%lluKB\n", tag, flex);
  if (length > 0)
    (void)write(fd, line, static_cast<std::size_t>(length < static_cast<int>(sizeof(line))
                                                       ? length
                                                       : static_cast<int>(sizeof(line))));
  close(fd);
}

void Lock(std::atomic_flag& flag) noexcept
{
  while (flag.test_and_set(std::memory_order_acquire))
    std::this_thread::yield();
}

void Unlock(std::atomic_flag& flag) noexcept
{
  flag.clear(std::memory_order_release);
}

void LockShard(std::size_t shard) noexcept
{
  while (g_shardLocks[shard].exchange(true, std::memory_order_acquire))
    std::this_thread::yield();
}

void UnlockShard(std::size_t shard) noexcept
{
  g_shardLocks[shard].store(false, std::memory_order_release);
}

bool MapEnabled()
{
  static const bool enabled = []()
  {
    if (std::getenv("PS4_MEM_MAP") != nullptr)
      return true;
    return access("/data/kodi-ps4/mem-map", F_OK) == 0;
  }();
  return enabled;
}

struct MapHash
{
  std::size_t shard;
  std::size_t index;
};

MapHash HashPointer(const void* ptr)
{
  std::uintptr_t x = reinterpret_cast<std::uintptr_t>(ptr) >> 4;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return {static_cast<std::size_t>(x) & (MAP_SHARDS - 1),
          static_cast<std::size_t>(x >> 6) & (SHARD_SLOTS - 1)};
}

AllocEntry& EntryAt(const MapHash& hash, std::size_t probe)
{
  return g_entries[hash.shard * SHARD_SLOTS +
                   ((hash.index + probe) & (SHARD_SLOTS - 1))];
}

bool IsTombstone(const void* ptr)
{
  return reinterpret_cast<std::uintptr_t>(ptr) == TOMBSTONE;
}

void Track(void* ptr, std::size_t size, const void* caller)
{
  if (!ptr || !MapEnabled())
    return;

  const MapHash hash = HashPointer(ptr);
  LockShard(hash.shard);
  std::size_t firstTombstone = SHARD_SLOTS;
  std::size_t empty = SHARD_SLOTS;
  for (std::size_t i = 0; i < MAX_PROBES; ++i)
  {
    AllocEntry& entry = EntryAt(hash, i);
    if (!entry.ptr)
    {
      empty = (hash.index + i) & (SHARD_SLOTS - 1);
      break;
    }
    if (IsTombstone(entry.ptr))
    {
      if (firstTombstone == SHARD_SLOTS)
        firstTombstone = (hash.index + i) & (SHARD_SLOTS - 1);
      continue;
    }
    if (entry.ptr == ptr)
    {
      entry.size = size;
      entry.caller = caller;
      UnlockShard(hash.shard);
      return;
    }
  }

  const std::size_t slot = firstTombstone != SHARD_SLOTS ? firstTombstone : empty;
  if (slot == SHARD_SLOTS)
  {
    g_full.fetch_add(1, std::memory_order_relaxed);
    UnlockShard(hash.shard);
    return;
  }
  g_entries[hash.shard * SHARD_SLOTS + slot] = {ptr, size, caller};
  UnlockShard(hash.shard);
}

void Untrack(void* ptr)
{
  if (!ptr || !MapEnabled())
    return;

  const MapHash hash = HashPointer(ptr);
  LockShard(hash.shard);
  for (std::size_t i = 0; i < MAX_PROBES; ++i)
  {
    AllocEntry& entry = EntryAt(hash, i);
    if (!entry.ptr)
      break;
    if (IsTombstone(entry.ptr))
      continue;
    if (entry.ptr == ptr)
    {
      entry.ptr = reinterpret_cast<void*>(TOMBSTONE);
      entry.size = 0;
      entry.caller = nullptr;
      UnlockShard(hash.shard);
      return;
    }
  }
  g_untrackedFrees.fetch_add(1, std::memory_order_relaxed);
  UnlockShard(hash.shard);
}

void WriteMapLog(const char* data, std::size_t length)
{
  if (length == 0)
    return;

  Lock(g_logLock);
  int fd = open(MAP_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0)
  {
    if (g_logBytes + length > MAP_LOG_LIMIT)
    {
      close(fd);
      rename(MAP_LOG, MAP_OLD_LOG);
      fd = open(MAP_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      g_logBytes = 0;
    }
    if (fd >= 0)
    {
      (void)write(fd, data, length);
      close(fd);
      g_logBytes += length;
    }
  }
  Unlock(g_logLock);
}

void StartMap()
{
  bool expected = false;
  if (!MapEnabled() || !g_mapStarted.compare_exchange_strong(expected, true))
    return;

  Lock(g_logLock);
  rename(MAP_LOG, MAP_OLD_LOG);
  int fd = open(MAP_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0)
  {
    char header[96];
    const int length = snprintf(header, sizeof(header),
                                "=== mem-map session pid=%d t=%lld slots=%zu probes=%zu shards=%zu ===\n",
                                static_cast<int>(getpid()),
                                static_cast<long long>(time(nullptr)),
                                MAP_SLOTS, MAX_PROBES, MAP_SHARDS);
    if (length > 0)
    {
      const std::size_t total = static_cast<std::size_t>(length) < sizeof(header)
                                    ? static_cast<std::size_t>(length)
                                    : sizeof(header) - 1;
      (void)write(fd, header, total);
      g_logBytes = total;
    }
    close(fd);
  }
  Unlock(g_logLock);
}

struct CallerStat
{
  const void* caller;
  std::size_t bytes;
  std::size_t count;
};

} // namespace

void PS4MemoryReserveInit()
{
  if (g_memoryReserve.load(std::memory_order_acquire) != nullptr ||
      g_heapReserveActive.load(std::memory_order_acquire))
    return;

  const std::size_t bytes = MemoryReserveBytes();
  /* Split the emergency budget: the heap part is returned to libc malloc by
   * operator new (small std::string/JSON allocations), the kernel part is
   * unmapped for pthread_create/mmap (2 MiB thread stacks).  With the
   * default 64 MB this is 32 MB heap + 32 MB kernel; the 16 MB heap split of
   * 0.1.1an ran out during the Home/PVR texture load. */
  const std::size_t heapBytes = bytes / 2;
  const std::size_t kernelBytes = bytes - heapBytes;

  if (heapBytes != 0)
  {
    const std::size_t wanted = std::min(heapBytes / HEAP_RESERVE_CHUNK,
                                        MAX_HEAP_RESERVE_CHUNKS);
    std::size_t allocated = 0;
    for (; allocated < wanted; ++allocated)
    {
      void* chunk = std::malloc(HEAP_RESERVE_CHUNK);
      if (!chunk)
        break;
      std::memset(chunk, 0, HEAP_RESERVE_CHUNK);
      auto* words = static_cast<std::uint64_t*>(chunk);
      words[0] = HEAP_RESERVE_MAGIC;
      words[HEAP_RESERVE_CHUNK / sizeof(std::uint64_t) - 1] = HEAP_RESERVE_MAGIC;
      g_heapReserveChunks[allocated] = chunk;
    }

    if (allocated == 0)
    {
      WriteReserveLog("init-heap-failed");
    }
    else
    {
      g_heapReserveCount.store(allocated, std::memory_order_release);
      g_heapReserveSize.store(allocated * HEAP_RESERVE_CHUNK,
                              std::memory_order_release);
      g_heapReserveActive.store(true, std::memory_order_release);
      WriteReserveLog("init-heap");
    }
  }

  if (kernelBytes != 0)
  {
    void* reserve = nullptr;
#if defined(__ORBIS__)
    /* Map from the kernel flexible pool, not malloc: freeing the reserve must
     * return pages to the pool so pthread_create/mmap can use them. */
    const int rc = sceKernelMapFlexibleMemory(&reserve, kernelBytes, 0x1 | 0x2, 0);
    if (rc != 0 || !reserve)
    {
      WriteReserveLog("init-failed");
      return;
    }
#else
    reserve = std::malloc(kernelBytes);
    if (!reserve)
    {
      WriteReserveLog("init-failed");
      return;
    }
#endif

    /* Touch every page so the flexible pool really reserves the physical
     * memory; an untouched block would not relieve pressure later. */
    std::memset(reserve, 0, kernelBytes);
    if (kernelBytes >= 2 * sizeof(std::uint64_t))
    {
      auto* words = static_cast<std::uint64_t*>(reserve);
      words[0] = KERNEL_RESERVE_MAGIC;
      words[kernelBytes / sizeof(std::uint64_t) - 1] = KERNEL_RESERVE_MAGIC;
    }

    void* expected = nullptr;
    if (!g_memoryReserve.compare_exchange_strong(expected, reserve,
                                                 std::memory_order_acq_rel))
    {
#if defined(__ORBIS__)
      sceKernelMunmap(reserve, kernelBytes);
#else
      std::free(reserve);
#endif
      return;
    }
    g_memoryReserveSize.store(kernelBytes, std::memory_order_release);
    WriteReserveLog("init");
  }
}

bool PS4HeapReserveRelease()
{
  if (!g_heapReserveActive.exchange(false, std::memory_order_acq_rel))
    return false;

  const std::size_t count = g_heapReserveCount.exchange(0, std::memory_order_acq_rel);
  const std::size_t bytes = g_heapReserveSize.exchange(0, std::memory_order_acq_rel);
  bool intact = true;

  /* Free in reverse allocation order so the chunks coalesce at the top of
   * the sbrk heap.  Each chunk carries PS4HRESE at both ends. */
  for (std::size_t i = count; i-- > 0;)
  {
    void* chunk = g_heapReserveChunks[i];
    g_heapReserveChunks[i] = nullptr;
    if (!chunk)
    {
      intact = false;
      continue;
    }
    const auto* words = static_cast<const std::uint64_t*>(chunk);
    if (words[0] != HEAP_RESERVE_MAGIC ||
        words[HEAP_RESERVE_CHUNK / sizeof(std::uint64_t) - 1] != HEAP_RESERVE_MAGIC)
      intact = false;
    std::free(chunk);
  }

  (void)bytes;
  WriteReserveLog(intact ? "release-heap" : "release-heap-corrupt");
  return true;
}

bool PS4MemoryReserveRelease()
{
  void* reserve = g_memoryReserve.exchange(nullptr, std::memory_order_acq_rel);
  if (!reserve)
    return false;
  const std::size_t bytes = g_memoryReserveSize.exchange(0, std::memory_order_acq_rel);

  bool intact = true;
  if (bytes >= 2 * sizeof(std::uint64_t))
  {
    const auto* words = static_cast<const std::uint64_t*>(reserve);
    intact = words[0] == KERNEL_RESERVE_MAGIC &&
             words[bytes / sizeof(std::uint64_t) - 1] == KERNEL_RESERVE_MAGIC;
  }

#if defined(__ORBIS__)
  if (bytes != 0)
    (void)sceKernelMunmap(reserve, bytes);
#else
  std::free(reserve);
#endif
  WriteReserveLog(intact ? "release" : "release-corrupt");
  return true;
}

/* libc++ declares these weak; these strong definitions take over and record
 * the requested size and the immediate caller.  malloc/free is used so the
 * trace does not depend on operator new itself. */
void* operator new(std::size_t size)
{
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr && size <= RESERVE_RELEASE_MAX)
  {
    if (PS4HeapReserveRelease())
      memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr && PS4MemoryReserveRelease())
      memory = std::malloc(size == 0 ? 1 : size);
  }
  if (memory == nullptr)
  {
    PS4AllocFailureWrite(size, __builtin_return_address(0));
    throw std::bad_alloc();
  }

  if (size >= BIG_ALLOCATION_THRESHOLD)
    PS4AllocTraceWrite(size, __builtin_return_address(0));
  Track(memory, size, __builtin_return_address(0));

  return memory;
}

void* operator new[](std::size_t size)
{
  return ::operator new(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
  void* memory = nullptr;
  const std::size_t align = static_cast<std::size_t>(alignment);
  int result = posix_memalign(&memory, align, size == 0 ? 1 : size);
  if (result != 0 && size <= RESERVE_RELEASE_MAX)
  {
    if (PS4HeapReserveRelease())
      result = posix_memalign(&memory, align, size == 0 ? 1 : size);
    if (result != 0 && PS4MemoryReserveRelease())
      result = posix_memalign(&memory, align, size == 0 ? 1 : size);
  }
  if (result != 0)
  {
    PS4AllocFailureWrite(size, __builtin_return_address(0));
    throw std::bad_alloc();
  }

  if (size >= BIG_ALLOCATION_THRESHOLD)
    PS4AllocTraceWrite(size, __builtin_return_address(0));
  Track(memory, size, __builtin_return_address(0));
  return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
  return ::operator new(size, alignment);
}

void operator delete(void* memory) noexcept
{
  Untrack(memory);
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
  ::operator delete(memory);
}

void operator delete[](void* memory) noexcept
{
  ::operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
  ::operator delete(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
  ::operator delete(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
  ::operator delete(memory);
}

void PS4MemMapStart()
{
  StartMap();
}

void PS4MemMapSnapshot(const char* tag)
{
  if (!MapEnabled())
    return;
  StartMap();

  CallerStat top[MAX_CALLERS] = {};
  std::size_t topCount = 0;
  std::size_t live = 0;
  std::size_t allocs = 0;
  std::size_t untracked = 0;
  std::size_t full = 0;
  long long delta = 0;

  for (std::size_t shard = 0; shard < MAP_SHARDS; ++shard)
  {
    LockShard(shard);
    for (std::size_t j = 0; j < SHARD_SLOTS; ++j)
    {
      const AllocEntry& entry = g_entries[shard * SHARD_SLOTS + j];
      if (!entry.ptr || IsTombstone(entry.ptr))
        continue;

      live += entry.size;
      ++allocs;
      std::size_t slot = 0;
      for (; slot < topCount; ++slot)
      {
        if (top[slot].caller == entry.caller)
          break;
      }
      if (slot == topCount)
      {
        if (topCount < MAX_CALLERS)
        {
          top[topCount].caller = entry.caller;
          top[topCount].bytes = 0;
          top[topCount].count = 0;
          ++topCount;
        }
        else
        {
          /* Keep the list bounded; the largest allocations are the useful
           * signal.  A caller that only allocates small blocks after the
           * table is full may not appear, so the total is logged separately. */
          continue;
        }
      }
      top[slot].bytes += entry.size;
      ++top[slot].count;
    }
    UnlockShard(shard);
  }
  untracked = g_untrackedFrees.load(std::memory_order_relaxed);
  full = g_full.load(std::memory_order_relaxed);
  const std::size_t previous = g_lastLive.exchange(live, std::memory_order_relaxed);
  delta = static_cast<long long>(live) - static_cast<long long>(previous);

  /* Sort by live bytes, descending. */
  for (std::size_t i = 0; i < topCount; ++i)
  {
    for (std::size_t j = i + 1; j < topCount; ++j)
    {
      if (top[j].bytes > top[i].bytes)
      {
        const CallerStat tmp = top[i];
        top[i] = top[j];
        top[j] = tmp;
      }
    }
  }

  char buffer[4096];
  int length = snprintf(buffer, sizeof(buffer),
                        "PS4MEMMAP t=%lld tag=%s live=%zuKB allocs=%zu "
                        "delta=%+lldKB untracked=%zu full=%zu\n",
                        static_cast<long long>(time(nullptr)), tag ? tag : "?",
                        live / 1024, allocs, delta / 1024, untracked, full);
  if (length <= 0)
    return;
  if (length > static_cast<int>(sizeof(buffer) - 1))
    length = static_cast<int>(sizeof(buffer) - 1);

  for (std::size_t i = 0; i < topCount && length < static_cast<int>(sizeof(buffer) - 96); ++i)
  {
    const int n = snprintf(buffer + length, sizeof(buffer) - static_cast<std::size_t>(length),
                           "  caller=0x%llx bytes=%zuKB allocs=%zu\n",
                           static_cast<unsigned long long>(
                               reinterpret_cast<std::uintptr_t>(top[i].caller)),
                           top[i].bytes / 1024, top[i].count);
    if (n <= 0)
      break;
    length += n;
  }
  WriteMapLog(buffer, static_cast<std::size_t>(length));
}
