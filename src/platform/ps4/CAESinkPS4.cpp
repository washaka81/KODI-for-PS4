#include "CAESinkPS4.h"
#include "PS4AudioSink.h"

namespace kodi::ps4 {

bool CAESinkPS4::Initialize(uint32_t sample_rate, uint32_t channels) {
  sample_rate_ = sample_rate;
  channels_ = channels;
  return (ready_ = sink_.Initialize(sample_rate_, channels_));
}

bool CAESinkPS4::OutputPCM(const void* data, std::size_t bytes) {
  if (!ready_) return false;
  if (bytes == 0U) return true;
  sink_.Submit(data, bytes);
  return true;
}

}  // namespace kodi::ps4