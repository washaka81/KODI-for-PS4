#pragma once

#include <cstdint>
#include <string>

namespace kodi::ps4 {

class PlatformPS4 {
 public:
  bool Initialize();
  void Shutdown();

  bool IsJailbroken() const { return jailbroken_; }

 private:
  bool jailbroken_ = false;
  bool system_services_ready_ = false;
};

}  // namespace kodi::ps4
