#pragma once

#include <cstdint>

namespace kodi::ps4 {

class PS4RendererGLES2 {
 public:
  bool Initialize(uint32_t width = 1920, uint32_t height = 1080);
  void RenderFrame();
  void Shutdown();

  uint32_t Width() const { return width_; }
  uint32_t Height() const { return height_; }

 private:
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;
  void* display_ = nullptr;
  void* context_ = nullptr;
  void* surface_ = nullptr;
};

}  // namespace kodi::ps4
