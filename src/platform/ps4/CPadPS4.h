#pragma once

#include "PS4Input.h"

namespace kodi::ps4 {

class CPadPS4 {
 public:
  bool Initialize() { return input_.Initialize(); }
  void Poll() { input_.Poll(); }
  PS4InputState State() const { return input_.State(); }

 private:
  PS4Input input_;
};

}  // namespace kodi::ps4