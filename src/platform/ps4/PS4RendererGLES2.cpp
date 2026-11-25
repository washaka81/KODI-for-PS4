#include <cstdio>
#include "PS4RendererGLES2.h"

#include <cstring>
#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/libkernel.h>
}
#endif

#if defined(__ORBIS__) || defined(PS4)
extern "C" {
#include <orbis/Pigletv2VSH.h>
}
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#endif

namespace kodi::ps4 {

bool PS4RendererGLES2::Initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

#if defined(__ORBIS__) || defined(PS4)
  const char *sandbox = sceKernelGetFsSandboxRandomWord();
  char pigletname[256] = {0};
  char precompiledname[256] = {0};
  if (sandbox) {
    snprintf(pigletname, sizeof(pigletname), "/%s/common/lib/libScePigletv2VSH.sprx", sandbox);
    snprintf(precompiledname, sizeof(precompiledname), "/%s/common/lib/libScePrecompiledShaders.sprx", sandbox);
  }

  FILE *lfm = fopen("/data/kodi-ps4/init.log","a");
  if (lfm) { fprintf(lfm, "sandbox=%s\n", sandbox ? sandbox : "(null)\n"); fprintf(lfm, "piglet_sprx=%s\n", pigletname); fflush(lfm); fclose(lfm); }

  int mstart = 0;
  uint32_t piglet_mod = 0;
  uint32_t precomp_mod = 0;
  if (sandbox) {
    piglet_mod = sceKernelLoadStartModule(pigletname, 0, nullptr, 0, nullptr, &mstart);
    if ((int)piglet_mod < 0) {
      FILE *lf = fopen("/data/kodi-ps4/init.log","a");
      if (lf){fprintf(lf, "piglet_module_load_failed=0x%x\n", piglet_mod);fflush(lf);fclose(lf);}
      return false;
    }
    precomp_mod = sceKernelLoadStartModule(precompiledname, 0, nullptr, 0, nullptr, &mstart);
    if ((int)precomp_mod < 0) {
      FILE *lf = fopen("/data/kodi-ps4/init.log","a");
      if (lf){fprintf(lf, "precomp_module_load_failed=0x%x\n", precomp_mod);fflush(lf);fclose(lf);}
      return false;
    }
    FILE *lf2 = fopen("/data/kodi-ps4/init.log","a");
    if (lf2){fprintf(lf2, "modules_ok piglet=0x%x precomp=0x%x\n", piglet_mod, precomp_mod);fflush(lf2);fclose(lf2);}
  }

  OrbisPglConfig config{};
  // La superficie EGL guarda el puntero a la ventana, asi que no puede ser una
  // variable local: al salir de esta funcion el puntero quedaria colgando y
  // el compositor no presentaria nada (pantalla negra).
  static OrbisPglWindow window{0, static_cast<khronos_uint32_t>(width_), static_cast<khronos_uint32_t>(height_)};
  window.uWidth = static_cast<khronos_uint32_t>(width_);
  window.uHeight = static_cast<khronos_uint32_t>(height_);


  std::memset(&config, 0, sizeof(config));
  config.size = sizeof(config);
  config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
  config.processOrder = 1;
  config.systemSharedMemorySize = 250 * 1024 * 1024;
  config.videoSharedMemorySize = 512 * 1024 * 1024;
  config.maxMappedFlexibleMemory = 170 * 1024 * 1024;
  config.drawCommandBufferSize = 1 * 1024 * 1024;
  config.lcueResourceBufferSize = 1 * 1024 * 1024;
  config.dbgPosCmd_0x40 = width_;
  config.dbgPosCmd_0x44 = height_;
  config.dbgPosCmd_0x48 = 0;
  config.dbgPosCmd_0x4C = 0;
  config.unk_0x5C = 2;

  if (!scePigletSetConfigurationVSH(&config)) {
    FILE *lf = fopen("/data/kodi-ps4/init.log","a");
    if (lf){fputs("piglet_config_fail\n",lf);fflush(lf);fclose(lf);}
    return false;
  }

  const EGLint attribs[] = {
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLE_BUFFERS, 0,
      EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};

  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

  const EGLint surfaceAttribs[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};

  EGLDisplay found = EGL_NO_DISPLAY;
  EGLint found_id = -1;
  for (EGLint disp_id = 0; disp_id <= 7; ++disp_id) {
    EGLDisplay tryd = eglGetDisplay((EGLNativeDisplayType)disp_id);
    EGLint prev_err = eglGetError();
    if (tryd != EGL_NO_DISPLAY) { found = tryd; found_id = disp_id; break; }
    char tmp[96];
    snprintf(tmp,sizeof(tmp)," EGL_GETDISPLAY_%d=0x%p ERR=0x%x\n",(int)disp_id,(void*)tryd,(unsigned)prev_err);
    FILE*lf=fopen("/data/kodi-ps4/init.log","a");
    if (lf){fputs(tmp,lf);fflush(lf);fclose(lf);}
  }
  EGLDisplay d = found;
  if (d == EGL_NO_DISPLAY) { FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("egl_nodisplay_all\n",lf);fflush(lf);fclose(lf);} return false; }
  {
    FILE *lf=fopen("/data/kodi-ps4/init.log","a");
    if (lf){char tmp[64]; snprintf(tmp,sizeof(tmp),"egl_display_ok id=%d ptr=0x%p\n",found_id,(void*)d); fputs(tmp,lf);fflush(lf);fclose(lf);}
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(d, &major, &minor)) { FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("egl_init_fail\n",lf);fflush(lf);fclose(lf);} return false; }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;
  eglSwapInterval(d, 0);

  EGLConfig econfig = nullptr;
  EGLint numConfigs = 0;
  if (!eglChooseConfig(d, attribs, &econfig, 1, &numConfigs) || numConfigs < 1) return false;

  EGLSurface surf = eglCreateWindowSurface(d, econfig, &window, surfaceAttribs);
  if (surf == EGL_NO_SURFACE) { FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("egl_surface_fail\n",lf);fflush(lf);fclose(lf);} return false; }

  EGLContext ctx = eglCreateContext(d, econfig, EGL_NO_CONTEXT, ctxAttribs);
  if (ctx == EGL_NO_CONTEXT) { FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("egl_context_fail\n",lf);fflush(lf);fclose(lf);} return false; }

  if (!eglMakeCurrent(d, surf, surf, ctx)) { FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("egl_makecurrent_fail\n",lf);fflush(lf);fclose(lf);} return false; }

  display_ = (void*)d;
  surface_ = (void*)surf;
  context_ = (void*)ctx;

  FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("renderer_init_ok\n",lf);fflush(lf);fclose(lf);}
  initialized_ = true;
  return true;
#else
  FILE*lf=fopen("/data/kodi-ps4/init.log","a"); if(lf){fputs("renderer_init_ok\n",lf);fflush(lf);fclose(lf);}
  initialized_ = true;
  return true;
#endif
}

void PS4RendererGLES2::RenderFrame() {
  if (!initialized_) return;
#if defined(__ORBIS__) || defined(PS4)
  EGLDisplay d = (EGLDisplay)display_;
  EGLSurface s = (EGLSurface)surface_;
  if (d == nullptr || s == nullptr) return;
  glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glFlush();
  glFinish();
  eglSwapBuffers(d, s);
#endif
}

void PS4RendererGLES2::Shutdown() {
#if defined(__ORBIS__) || defined(PS4)
  EGLDisplay d = (EGLDisplay)display_;
  if (d != nullptr) {
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_) eglDestroySurface(d, (EGLSurface)surface_);
    if (context_) eglDestroyContext(d, (EGLContext)context_);
    eglTerminate(d);
  }
  display_ = nullptr;
  surface_ = nullptr;
  context_ = nullptr;
#endif
  initialized_ = false;
}

}  // namespace kodi::ps4
