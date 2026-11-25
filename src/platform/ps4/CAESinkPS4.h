#pragma once

#include <cstdint>
#include <cstddef>

#include "PS4AudioSink.h"

namespace kodi::ps4 {

class CAESinkPS4 {
 public:
  bool Initialize(uint32_t sample_rate, uint32_t channels);
  bool OutputPCM(const void* data, std::size_t bytes);
  bool IsReady() const { return ready_; }

 private:
  PS4AudioSink sink_;
  bool ready_ = false;
  uint32_t sample_rate_ = 48000;
  uint32_t channels_ = 2;
};

}  // namespace kodi::ps4