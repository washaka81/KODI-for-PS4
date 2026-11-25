#pragma once

#include <cstdint>

namespace kodi::ps4 {

struct PS4InputState {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  bool confirm = false;
  bool back = false;
  bool play_pause = false;
  float stick_x = 0.0f;
  float stick_y = 0.0f;
};

class PS4Input {
 public:
  bool Initialize();
  void Poll();
  PS4InputState State() const { return state_; }
  void Shutdown();

 private:
  PS4InputState state_{};
  bool initialized_ = false;
  int32_t pad_handle_ = -1;
};

}  // namespace kodi::ps4
