/*
 * eglshim — minimal EGL 1.4 surface/context over the glshim GNM engine.
 *
 * Kodi's PS4 window system only needs one display, one window surface and one
 * GLES2 context.  eglInitialize brings the GNM engine up; eglSwapBuffers
 * submits the command buffer and flips the VideoOut buffer.
 */
#include <EGL/egl.h>

#include <string.h>

#include "glshim.h"

static int g_inited;
static EGLint g_error = EGL_SUCCESS;
static EGLint g_interval;
static EGLDisplay g_current_display = EGL_NO_DISPLAY;
static EGLSurface g_current_draw = EGL_NO_SURFACE;
static EGLSurface g_current_read = EGL_NO_SURFACE;
static EGLContext g_current_context = EGL_NO_CONTEXT;

static void set_error(EGLint e) {
	if (g_error == EGL_SUCCESS)
		g_error = e;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
	(void)display_id;
	return (EGLDisplay)(uintptr_t)1;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
	if (dpy == EGL_NO_DISPLAY) {
		set_error(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}
	if (!g_inited) {
		/* Open VideoOut before Kodi hides the system splash: the probe
		 * hides it right before the first clear, and opening the port
		 * after the hide blocks in sceVideoOutOpen on this firmware. */
		glShimLog("egl: initialize (eager engine)");
		glShimTrace("shim-egl-init-begin");
		if (!glShimInit(1920, 1080)) {
			glShimLog("egl: glShimInit failed");
			glShimTrace("shim-egl-init-fail");
			set_error(EGL_NOT_INITIALIZED);
			return EGL_FALSE;
		}
		glShimLog("egl: glShimInit ok");
		glShimTrace("shim-egl-init-ok");
		g_inited = 1;
	}
	if (major)
		*major = 2;
	if (minor)
		*minor = 0;
	return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
	if (dpy == EGL_NO_DISPLAY) {
		set_error(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}
	if (g_inited) {
		glShimShutdown();
		g_inited = 0;
	}
	g_current_display = EGL_NO_DISPLAY;
	g_current_draw = EGL_NO_SURFACE;
	g_current_read = EGL_NO_SURFACE;
	g_current_context = EGL_NO_CONTEXT;
	return EGL_TRUE;
}

const char* eglQueryString(EGLDisplay dpy, EGLint name) {
	(void)dpy;
	switch (name) {
	case EGL_VENDOR:
		return "Kodi PS4";
	case EGL_VERSION:
		return "1.4 glshim";
	case EGL_CLIENT_APIS:
		return "OpenGL_ES";
	case EGL_EXTENSIONS:
		return "";
	default:
		set_error(EGL_BAD_PARAMETER);
		return NULL;
	}
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs,
			   EGLint config_size, EGLint* num_config) {
	(void)dpy;
	(void)attrib_list;
	if (num_config)
		*num_config = 1;
	if (configs && config_size > 0)
		configs[0] = (EGLConfig)(uintptr_t)1;
	return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute,
			      EGLint* value) {
	(void)dpy;
	(void)config;
	if (!value) {
		set_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	switch (attribute) {
	case EGL_CONFIG_ID:
		*value = 1;
		break;
	case EGL_RED_SIZE:
	case EGL_GREEN_SIZE:
	case EGL_BLUE_SIZE:
	case EGL_ALPHA_SIZE:
		*value = 8;
		break;
	case EGL_DEPTH_SIZE:
	case EGL_STENCIL_SIZE:
	case EGL_SAMPLE_BUFFERS:
	case EGL_SAMPLES:
		*value = 0;
		break;
	case EGL_RENDERABLE_TYPE:
		*value = EGL_OPENGL_ES2_BIT;
		break;
	case EGL_SURFACE_TYPE:
		*value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
		break;
	case EGL_TRANSPARENT_TYPE:
		*value = EGL_NONE;
		break;
	default:
		*value = 0;
		break;
	}
	return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
				  EGLNativeWindowType win, const EGLint* attrib_list) {
	(void)dpy;
	(void)config;
	(void)win;
	(void)attrib_list;
	glShimLog("egl: create window surface");
	return (EGLSurface)(uintptr_t)1;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
				   const EGLint* attrib_list) {
	(void)dpy;
	(void)config;
	(void)attrib_list;
	return (EGLSurface)(uintptr_t)2;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
	(void)dpy;
	(void)surface;
	return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
			    const EGLint* attrib_list) {
	(void)dpy;
	(void)config;
	(void)share_context;
	(void)attrib_list;
	return (EGLContext)(uintptr_t)1;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
	(void)dpy;
	(void)ctx;
	return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
			  EGLContext ctx) {
	(void)dpy;
	glShimLog("egl: make current");
	g_current_display = dpy;
	g_current_draw = draw;
	g_current_read = read;
	g_current_context = ctx;
	return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
	(void)dpy;
	(void)surface;
	glShimPresent();
	return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
	(void)dpy;
	g_interval = interval;
	return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
			   EGLint* value) {
	(void)dpy;
	(void)surface;
	if (!value) {
		set_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	switch (attribute) {
	case EGL_WIDTH:
		*value = 1920;
		break;
	case EGL_HEIGHT:
		*value = 1080;
		break;
	case EGL_RENDER_BUFFER:
		*value = EGL_BACK_BUFFER;
		break;
	case EGL_SWAP_BEHAVIOR:
		*value = EGL_BUFFER_DESTROYED;
		break;
	default:
		*value = 0;
		break;
	}
	return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api) {
	if (api != EGL_OPENGL_ES_API) {
		set_error(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}
	return EGL_TRUE;
}

EGLenum eglQueryAPI(void) { return EGL_OPENGL_ES_API; }

EGLDisplay eglGetCurrentDisplay(void) { return g_current_display; }
EGLContext eglGetCurrentContext(void) { return g_current_context; }
EGLSurface eglGetCurrentSurface(EGLint readdraw) {
	return readdraw == EGL_READ ? g_current_read : g_current_draw;
}

EGLBoolean eglReleaseThread(void) { return EGL_TRUE; }

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname) {
	(void)procname;
	return NULL;
}

EGLint eglGetError(void) {
	EGLint e = g_error;
	g_error = EGL_SUCCESS;
	return e;
}
