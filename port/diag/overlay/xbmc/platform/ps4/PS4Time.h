/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

#include "utils/TimeUtils.h"

namespace kodi::ps4
{

/*! \brief Monotonic wall-clock milliseconds.
 *
 * `std::steady_clock` tracks CPU time on Orbis, not wall time, so every PS4
 * helper uses the same CLOCK_MONOTONIC_RAW source as the rest of the port.
 */
inline uint64_t NowMs()
{
  return static_cast<uint64_t>(CurrentHostCounter() / 1000000);
}

} // namespace kodi::ps4
