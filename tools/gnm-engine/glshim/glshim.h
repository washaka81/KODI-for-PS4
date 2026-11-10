/*
 * glshim v1 — GLES2/EGL subset over sceGnm for the Kodi PS4 port.
 *
 * The public API matches the Khronos GLES2/EGL entry points so Kodi can be
 * linked against libglshim.a instead of libScePigletv2VSH.  The shim owns the
 * VideoOut display: eglInitialize brings the engine up and eglSwapBuffers
 * submits the GNM command buffer and flips.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GLES3 core entry points that the SDK's GLES2 headers do not declare */
/* ------------------------------------------------------------------ */

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#endif
#ifndef GL_FRAMEBUFFER_UNSUPPORTED
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#endif
#ifndef GL_READ_BUFFER
#define GL_READ_BUFFER 0x0C02
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1F03
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#endif
#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#endif
#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_VALIDATE_STATUS
#define GL_VALIDATE_STATUS 0x8B83
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif

GL_APICALL void GL_APIENTRY glGenVertexArrays(GLsizei n, GLuint* arrays);
GL_APICALL void GL_APIENTRY glBindVertexArray(GLuint array);
GL_APICALL void GL_APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
GL_APICALL const GLubyte* GL_APIENTRY glGetStringi(GLenum name, GLuint index);
GL_APICALL GLsync GL_APIENTRY glFenceSync(GLenum condition, GLbitfield flags);
GL_APICALL GLboolean GL_APIENTRY glIsSync(GLsync sync);
GL_APICALL void GL_APIENTRY glDeleteSync(GLsync sync);
GL_APICALL GLenum GL_APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
GL_APICALL void GL_APIENTRY glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
GL_APICALL void GL_APIENTRY glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                              GLbitfield mask, GLenum filter);
GL_APICALL void GL_APIENTRY glReadBuffer(GLenum src);

/* ------------------------------------------------------------------ */
/* Shim lifecycle (not part of GL; eglInitialize/eglSwapBuffers use it) */
/* ------------------------------------------------------------------ */
int glShimInit(unsigned width, unsigned height);
void glShimLog(const char* fmt, ...);
void glShimTrace(const char* stage);
void glShimShutdown(void);
void glShimPresent(void);
uint64_t glShimPresentCount(void);
uint64_t glShimGetProcessTimeUs(void);
void glShimWaitFlip(void);
uint64_t glShimGetVideoOutCapability(void);
uint64_t glShimGetFlipCount(void);

#ifdef __cplusplus
}
#endif
