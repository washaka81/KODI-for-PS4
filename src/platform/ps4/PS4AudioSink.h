#pragma once

#include <cstdint>
#include <cstddef>

namespace kodi::ps4 {

class PS4AudioSink {
 public:
  bool Initialize(uint32_t sample_rate = 48000, uint32_t channels = 2);
  void Submit(const void* pcm, std::size_t bytes);
  void SetVolume(float volume);
  void Shutdown();

 private:
  uint32_t sample_rate_ = 48000;
  uint32_t channels_ = 2;
  float volume_ = 1.0f;
  bool initialized_ = false;
  int32_t port_handle_ = 0;
};

}  // namespace kodi::ps4
