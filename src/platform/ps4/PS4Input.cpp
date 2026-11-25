#include "PS4Input.h"

#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/Pad.h>
#include <orbis/UserService.h>
}
#endif

#include <cstdio>

namespace kodi::ps4 {

bool PS4Input::Initialize() {
#if defined(__ORBIS__) || defined(PS4)
  int rc = scePadInit();
  if (rc != 0) {
    FILE *log = fopen("/data/kodi-ps4/input.log", "a");
    if (log) {
      fprintf(log, "scePadInit failed: 0x%x\n", rc);
      fclose(log);
    }
    return false;
  }

  // Open pad for the current user
  int32_t user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
  if (sceUserServiceGetInitialUser(&user_id) != 0) {
    user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
  }

  pad_handle_ = scePadOpen(
      user_id,
      ORBIS_PAD_PORT_TYPE_STANDARD,
      0,
      nullptr
  );

  if (pad_handle_ < 0) {
    FILE *log = fopen("/data/kodi-ps4/input.log", "a");
    if (log) {
      fprintf(log, "scePadOpen failed: 0x%x\n", pad_handle_);
      fclose(log);
    }
    return false;
  }

  initialized_ = true;

  FILE *log = fopen("/data/kodi-ps4/input.log", "a");
  if (log) {
    fprintf(log, "input_init_ok handle=%d\n", pad_handle_);
    fclose(log);
  }

  return true;
#else
  initialized_ = true;
  return true;
#endif
}

void PS4Input::Poll() {
  if (!initialized_) return;
#if defined(__ORBIS__) || defined(PS4)
  OrbisPadData pad_data;
  int rc = scePadReadState(pad_handle_, &pad_data);
  if (rc != 0) return;

  // Map DS4 buttons to our input state
  state_.up = (pad_data.buttons & ORBIS_PAD_BUTTON_UP) != 0;
  state_.down = (pad_data.buttons & ORBIS_PAD_BUTTON_DOWN) != 0;
  state_.left = (pad_data.buttons & ORBIS_PAD_BUTTON_LEFT) != 0;
  state_.right = (pad_data.buttons & ORBIS_PAD_BUTTON_RIGHT) != 0;
  state_.confirm = (pad_data.buttons & ORBIS_PAD_BUTTON_CROSS) != 0;
  state_.back = (pad_data.buttons & ORBIS_PAD_BUTTON_CIRCLE) != 0;
  state_.play_pause = (pad_data.buttons & ORBIS_PAD_BUTTON_OPTIONS) != 0;

  // Map left stick (values 0-255, center ~128)
  state_.stick_x = (static_cast<float>(pad_data.leftStick.x) - 128.0f) / 128.0f;
  state_.stick_y = (static_cast<float>(pad_data.leftStick.y) - 128.0f) / 128.0f;
#endif
}

void PS4Input::Shutdown() {
#if defined(__ORBIS__) || defined(PS4)
  if (pad_handle_ >= 0) {
    scePadClose(pad_handle_);
    pad_handle_ = -1;
  }
#endif
  initialized_ = false;
}

}  // namespace kodi::ps4
