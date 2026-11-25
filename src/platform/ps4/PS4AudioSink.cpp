#include "PS4AudioSink.h"

#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
}
#endif

#include <cstdio>

namespace kodi::ps4 {

bool PS4AudioSink::Initialize(uint32_t sample_rate, uint32_t channels) {
  sample_rate_ = sample_rate;
  channels_ = channels;

#if defined(__ORBIS__) || defined(PS4)
  // Initialize audio output system
  int rc = sceAudioOutInit();
  if (rc != 0) {
    FILE *log = fopen("/data/kodi-ps4/audio.log", "a");
    if (log) {
      fprintf(log, "sceAudioOutInit failed: 0x%x\n", rc);
      fclose(log);
    }
    return false;
  }

  // Open audio port for the current user
  // Parameters: userId, portType, index, length, sampleRate, paramType
  int32_t user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
  if (sceUserServiceGetInitialUser(&user_id) != 0) {
    user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
  }

  port_handle_ = sceAudioOutOpen(
      user_id,
      ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
      0,
      static_cast<int32_t>(channels * 1024),
      static_cast<int32_t>(sample_rate),
      channels == 1 ? ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO : ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
  );

  if (port_handle_ < 0) {
    FILE *log = fopen("/data/kodi-ps4/audio.log", "a");
    if (log) {
      fprintf(log, "sceAudioOutOpen failed: 0x%x\n", port_handle_);
      fclose(log);
    }
    return false;
  }

  initialized_ = true;

  FILE *log = fopen("/data/kodi-ps4/audio.log", "a");
  if (log) {
    fprintf(log, "audio_init_ok port=%d rate=%u ch=%u\n", port_handle_, sample_rate_, channels_);
    fclose(log);
  }

  return true;
#else
  initialized_ = true;
  return true;
#endif
}

void PS4AudioSink::Submit(const void* pcm, std::size_t bytes) {
  if (!initialized_ || !pcm || bytes == 0) return;
#if defined(__ORBIS__) || defined(PS4)
  int rc = sceAudioOutOutput(port_handle_, pcm);
  if (rc != 0) {
    FILE *log = fopen("/data/kodi-ps4/audio.log", "a");
    if (log) {
      fprintf(log, "sceAudioOutOutput failed: 0x%x\n", rc);
      fclose(log);
    }
  }
#else
  (void)pcm;
  (void)bytes;
#endif
}

void PS4AudioSink::SetVolume(float volume) {
  volume_ = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
#if defined(__ORBIS__) || defined(PS4)
  // Volume range: -128 to 0 (0 = max volume)
  int32_t vol = static_cast<int32_t>(-128.0f * (1.0f - volume_));
  int32_t vols[2] = {vol, vol};
  sceAudioOutSetVolume(port_handle_, 0, vols);
#endif
}

void PS4AudioSink::Shutdown() {
#if defined(__ORBIS__) || defined(PS4)
  if (port_handle_ >= 0) {
    sceAudioOutClose(port_handle_);
    port_handle_ = -1;
  }
#endif
  initialized_ = false;
}

}  // namespace kodi::ps4
