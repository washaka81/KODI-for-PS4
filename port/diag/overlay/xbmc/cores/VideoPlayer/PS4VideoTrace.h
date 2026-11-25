/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#if defined(TARGET_PS4)

#include <unistd.h>

namespace
{
/*! \brief True when /data/kodi-ps4/video-trace exists.
 *
 * Shared by the PS4 video decoder, the render manager and the video player
 * so the trace switch is read once and stays consistent.
 */
inline bool PS4VideoTrace()
{
  static const bool enabled = access("/data/kodi-ps4/video-trace", F_OK) == 0;
  return enabled;
}
} // namespace

#endif // TARGET_PS4
