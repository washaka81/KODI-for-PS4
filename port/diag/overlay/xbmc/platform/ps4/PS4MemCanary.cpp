/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS4MemCanary.h"

#if defined(__ORBIS__)

#include "PS4AllocTrace.h"
#include "ServiceBroker.h"
#include "GUILargeTextureManager.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/TextureManager.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/MemUtils.h"
#include "utils/log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <orbis/libkernel.h>

namespace
{
int g_memFd = -1;
bool g_memEnabled = false;
bool g_memStarted = false;

bool MemCanaryEnabled()
{
  if (getenv("PS4_MEM_CANARY") != nullptr)
    return true;
  return access("/data/kodi-ps4/mem-canary", F_OK) == 0;
}

long long LimitValue(int resource)
{
  struct rlimit limit = {};
  if (getrlimit(resource, &limit) != 0)
    return -1;
  if (limit.rlim_cur == RLIM_INFINITY)
    return -2;
  return static_cast<long long>(limit.rlim_cur);
}

/* /proc/self/statm is not reliable on Orbis (it returns 0), so keep the
 * getrusage value as a second, clearly labelled metric.  ru_maxrss is the
 * peak resident set, not the current one. */
uint64_t RssMaxBytes()
{
  struct rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0)
    return 0;
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
}

void StartFreshLog()
{
  (void)rename("/data/kodi-ps4/mem.log", "/data/kodi-ps4/mem.old.log");
}

void* MemCanaryThread(void*)
{
  for (;;)
  {
    PS4MemCanaryLog("tick");
    struct timespec ts = {1, 0};
    nanosleep(&ts, nullptr);
  }
  return nullptr;
}
} // namespace

void PS4MemCanaryStart()
{
  if (g_memStarted)
    return;
  g_memStarted = true;
  g_memEnabled = MemCanaryEnabled();
  if (!g_memEnabled)
    return;

  /* One file per session: the previous log is kept as mem.old.log so a
   * crash can be compared with the boot that produced it. */
  StartFreshLog();
  g_memFd = open("/data/kodi-ps4/mem.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (g_memFd < 0)
  {
    g_memEnabled = false;
    return;
  }

  PS4MemMapStart();

  pthread_attr_t attr;
  pthread_t thread;
  if (pthread_attr_init(&attr) == 0)
  {
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    if (pthread_create(&thread, &attr, MemCanaryThread, nullptr) != 0)
    {
      g_memEnabled = false;
      close(g_memFd);
      g_memFd = -1;
    }
    pthread_attr_destroy(&attr);
  }
  PS4MemCanaryLog("start");
}

void PS4MemCanaryLog(const char* tag)
{
  if (!g_memEnabled || g_memFd < 0)
    return;

  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

  /* Phase markers also record the virtual mapping of the stack, heap and
   * data segment: flexible/direct/stack/pooled/committed, range and name.
   * This distinguishes an exhausted heap from a fragmented/flexible one. */
  if (tag && strcmp(tag, "tick") != 0)
  {
    const auto page = [&](const char* label, const void* address)
    {
      if (!address || address == reinterpret_cast<void*>(-1))
        return;
      OrbisKernelVirtualQueryInfo info{};
      if (sceKernelVirtualQuery(address, 0, &info, sizeof(info)) != 0)
        return;
      char line[320];
      const int length = snprintf(
          line, sizeof(line),
          "PS4PAGE t=%lld tag=%s label=%s addr=%p start=%p end=%p off=0x%llx "
          "flex=%u direct=%u stack=%u pooled=%u committed=%u name=%.32s\n",
          static_cast<long long>(nowMs), tag, label, address, info.unk01, info.unk02,
          static_cast<unsigned long long>(info.offset), info.isFlexibleMemory,
          info.isDirectMemory, info.isStack, info.isPooledMemory, info.isCommitted,
          info.name);
      if (length > 0)
      {
        const size_t total = static_cast<size_t>(length) < sizeof(line)
                                 ? static_cast<size_t>(length)
                                 : sizeof(line) - 1;
        (void)write(g_memFd, line, total);
      }
    };
    page("stack", &tag);
    page("heap", sbrk(0));
    page("data", &g_memFd);
  }

  const uint64_t flex = KODI::MEMORY::GetAvailableFlexibleMemorySize();
  const uint64_t direct = KODI::MEMORY::GetAvailableDirectMemorySize();
  const uint64_t rss = KODI::MEMORY::GetCurrentRssBytes();
  const uint64_t rssMax = RssMaxBytes();
  const void* brkPtr = sbrk(0);
  char brk[32];
  if (brkPtr == reinterpret_cast<void*>(-1))
    snprintf(brk, sizeof(brk), "n/a");
  else
    snprintf(brk, sizeof(brk), "0x%llx",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(brkPtr)));
#ifdef RLIMIT_DATA
  const long long data = LimitValue(RLIMIT_DATA);
#else
  const long long data = -3;
#endif
#ifdef RLIMIT_AS
  const long long as = LimitValue(RLIMIT_AS);
#else
  const long long as = -3;
#endif

  char line[416];
  const int length = snprintf(
      line, sizeof(line),
      "PS4MEM t=%lld tag=%s flex=%lluKB direct=%lluKB rss=%lluKB rssmax=%lluKB "
      "brk=%s data=%lld as=%lld\n",
      static_cast<long long>(nowMs), tag ? tag : "?",
      static_cast<unsigned long long>(flex / 1024),
      static_cast<unsigned long long>(direct / 1024),
      static_cast<unsigned long long>(rss / 1024),
      static_cast<unsigned long long>(rssMax / 1024), brk, data, as);
  if (length > 0)
  {
    const size_t total = static_cast<size_t>(length) < sizeof(line)
                             ? static_cast<size_t>(length)
                             : sizeof(line) - 1;
    (void)write(g_memFd, line, total);
  }

  /* A memory map snapshot on every phase marker is cheap enough (the table
   * is fixed and the log is written once) and gives the delta that a pure
   * flex reading cannot show.  Ticks are excluded to avoid log spam. */
  if (tag && strcmp(tag, "tick") != 0)
    PS4MemMapSnapshot(tag);
}

void PS4MemoryReleaseForPlayback()
{
  const uint64_t flexible = KODI::MEMORY::GetAvailableFlexibleMemorySize();
  /* flex=0 is exactly when the release is needed; the old `flexible == 0`
   * early return skipped the cleanup under the worst pressure. */
  if (flexible >= 192ULL * 1024ULL * 1024ULL)
    return;

  /* Home/TextureManager cleanup takes the graphics context and must run on
   * the GUI thread.  CAddonInstallJob::DoWork runs on a CJobManager worker;
   * doing GUI work there can deadlock while the GUI thread waits for the
   * install job.  Skip it off-thread instead of risking a freeze. */
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (!messenger || !messenger->IsGUIThread())
  {
    PS4MemCanaryLog("playback-release-skip-non-gui");
    CLog::Log(LOGINFO,
              "PS4 playback: flex={} MB; skipping GUI release on non-GUI thread",
              flexible / (1024ULL * 1024ULL));
    return;
  }

  PS4MemCanaryLog("playback-release-before");
  CLog::Log(LOGINFO,
            "PS4 playback: flex={} MB; releasing Home/PVR resources before opening video",
            flexible / (1024ULL * 1024ULL));

  if (CGUIComponent* gui = CServiceBroker::GetGUI())
  {
    gui->GetLargeTextureManager().CleanupUnusedImages(true);
    gui->GetTextureManager().FreeUnusedTextures(0);
    if (CGUIWindow* home = gui->GetWindowManager().GetWindow(WINDOW_HOME))
      home->FreeResources(true);
  }

  PS4MemCanaryLog("playback-release-after");
}

#else

void PS4MemCanaryStart() {}
void PS4MemCanaryLog(const char*) {}

#endif
