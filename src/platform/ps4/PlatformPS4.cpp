#include "PlatformPS4.h"

#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
}
#endif

namespace kodi::ps4 {

bool PlatformPS4::Initialize() {
#if defined(__ORBIS__) || defined(PS4)
  // Piglet sample does not initialize user service before EGL.
  // Disable until needed to avoid side effects.
  jailbroken_ = true;
  system_services_ready_ = true;
#endif
  return system_services_ready_ || true;
}

void PlatformPS4::Shutdown() {
#if defined(__ORBIS__) || defined(PS4)
  // TODO: sceSysmoduleUnloadModule, sceSystemServiceTerminate.
#endif
  system_services_ready_ = false;
}

}  // namespace kodi::ps4
