/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>

/* Diagnostic only: when /data/kodi-ps4/mem-canary (or PS4_MEM_CANARY=1)
 * exists, a helper thread appends the flexible/direct memory, the program
 * break and the data/address-space limits to /data/kodi-ps4/mem.log once a
 * second.  Explicit PS4MemCanaryLog() calls mark the app phases (startup
 * window, PVR client, curl); the log is written with a pre-opened fd so it
 * survives the allocation pressure that triggers the trace. */
void PS4MemCanaryStart();
void PS4MemCanaryLog(const char* tag);

/* Release GUI/Home resources before opening a video when the flexible heap
 * is low.  The Home PVR widgets keep hundreds of channel/EPG items alive
 * while fullscreen video plays; the H.264 decoder then cannot allocate its
 * frame buffers.  This is a pressure-only path (flex < 192 MB). */
void PS4MemoryReleaseForPlayback();
