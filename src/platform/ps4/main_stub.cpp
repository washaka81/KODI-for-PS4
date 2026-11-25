#include "PlatformPS4.h"
#include "PS4RendererGLES2.h"

#include <cstdio>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
}
#endif

namespace
{
// Same handler as the one of the Kodi core, to check whether the signal
// handling of the PS4 C library works at all.
void CrashHandler(int sig, siginfo_t* info, void* context)
{
  char buffer[256];
  int length = snprintf(buffer, sizeof(buffer), "crash signal=%d code=%d addr=%p\n", sig,
                         info ? info->si_code : 0, info ? info->si_addr : nullptr);
  if (length > 0)
  {
    int fd = open("/data/kodi-ps4/render.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0)
    {
      ssize_t written = write(fd, buffer, length);
      (void)written;
      close(fd);
    }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

void CrashHandlerSimple(int sig)
{
  CrashHandler(sig, nullptr, nullptr);
}

void InstallCrashHandler()
{
  struct sigaction action = {};
  action.__sa_handler.__sa_sigaction = CrashHandler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);

  for (int sig : {SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGABRT})
  {
    sigaction(sig, &action, nullptr);
    signal(sig, CrashHandlerSimple);
  }
}
} // namespace

int main() {
  FILE *log = fopen("/data/kodi-ps4/render.log", "w");
  if (log) { fprintf(log, "start\n"); fflush(log); }

  InstallCrashHandler();
  if (log) { fprintf(log, "handler-installed\n"); fflush(log); }

  kodi::ps4::PlatformPS4 platform;
  bool ok = platform.Initialize();
  if (log) { fprintf(log, "platform_init=%d\n", ok ? 1 : 0); fflush(log); }
  if (!ok) {
    if (log) { fprintf(log, "platform_init_failed\n"); fclose(log); }
    return 1;
  }

  kodi::ps4::PS4RendererGLES2 renderer;
  bool r = renderer.Initialize();
  if (log) { fprintf(log, "renderer_init=%d\n", r ? 1 : 0); fflush(log); }
  if (!r) {
    if (log) fclose(log);
    return 1;
  }

#if defined(__ORBIS__) || defined(PS4)
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
  int splash_rc = sceSystemServiceHideSplashScreen();
  if (log) { fprintf(log, "splash_rc=0x%x\n", splash_rc); fflush(log); }
#endif

  if (log) { fprintf(log, "enter_loop\n"); fclose(log); }

  unsigned long frame = 0;
  for (;;) {
    renderer.RenderFrame();
    if ((frame % 50UL) == 0UL) {
      FILE *f = fopen("/data/kodi-ps4/render.log", "a");
      if (f) { fprintf(f, "frame=%lu\n", frame); fclose(f); }
    }
    ++frame;

    // A deliberate crash, to check that the handler of the C library of the
    // PS4 writes the trace before the process dies.
    if (frame == 100UL) {
      FILE *f = fopen("/data/kodi-ps4/render.log", "a");
      if (f) { fprintf(f, "deliberate-crash\n"); fclose(f); }
      volatile int* broken = nullptr;
      *broken = 1;
    }
  }

  renderer.Shutdown();
  platform.Shutdown();
  return 0;
}
