/*
 * glshim v1 — GLES2 subset over sceGnm for Kodi PS4.
 *
 * The GUI of Kodi speaks GLES2 (plus a few GLES3 entry points that are
 * stubbed for now).  This file implements that API on top of sceGnm and
 * sceVideoOut, without Piglet.  eglshim.c owns the EGL surface/context and
 * calls glShimInit()/glShimPresent().
 *
 * Draws are recorded into a GNM command buffer as they arrive.  Everything
 * the GPU reads (vertex views, uniform snapshots, descriptor sets) is copied
 * into a per-frame arena so later GL calls cannot corrupt earlier draws.
 */
#include "glshim.h"

#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

#include <gnm/drawcommandbuffer.h>
#include <gnm/gpuaddr/gpuaddr.h>
#include <gnm/platform.h>
#include <compat/freegnm.h>
#include <gnm/pssl/types.h>

#include "u/utility.h"
#include "clear_shader.h"
#include "displayctx.h"
#include "memalloc.h"
#include "misc.h"

#ifndef GL_RED
#define GL_RED 0x1903
#endif

#define MAX_TEXTURES 16384
#define SHIM_MAX_TEXTURE_SIZE 4096
#define SHIM_TEXTURE_CANARY 0x4E43584554345350ULL /* PS4TEXCN */
#define MAX_BUFFERS 2048
#define MAX_SHADERS 256
#define MAX_PROGRAMS 128
#define MAX_ATTRIBS 16
#define MAX_UNIS 128
#define MAX_SAMPLERS 16
#define MAX_ATTRS 32

#define FRAME_ARENA_SIZE (32u * 1024u * 1024u)
/* The async present pipeline builds frame N+1 while buffer N is being
 * scanned out.  Two buffers would overwrite the buffer that was just
 * flipped (tearing); three give the display a full vsync to finish with
 * a buffer before it is rendered again. */
#define FRAME_BUFFERS 3
#define GARBAGE_MAX 65536
#define CBUF_SIZE 4096

/* GL_LUMINANCE is sampled as .r, .g or .rgb depending on the shader.  A
 * plain R8 texture only fills .r, which breaks YV12 (U is read from .g) and
 * GUI icons that multiply texture.rgb.  Replicate X into RGB (RRR1) so the
 * one-byte storage keeps the GL_LUMINANCE contract. */
static const GnmDataFormat GNM_FMT_R8_RRR1_UNORM =
    GNM_DATA_FORMAT_INIT(GNM_IMG_DATA_FORMAT_8, GNM_IMG_NUM_FORMAT_UNORM, GNM_CHAN_X,
			 GNM_CHAN_X, GNM_CHAN_X, GNM_CHAN_CONSTANT1);

/* NV12 chroma: two bytes per texel (U,V).  Map the first byte to G and the
 * second to A so gles_pack.frag and the NV12 YUV2RGB shader keep reading
 * .g/.a; R and B are constant 0.  This halves chroma from RGBA8 (4 B) to
 * RG8 (2 B) without changing any shader.  Value: 0xA20003. */
static const GnmDataFormat GNM_FMT_R8G8_UV_GA_UNORM =
    GNM_DATA_FORMAT_INIT(GNM_IMG_DATA_FORMAT_8_8, GNM_IMG_NUM_FORMAT_UNORM,
			 GNM_CHAN_CONSTANT0, GNM_CHAN_X, GNM_CHAN_CONSTANT0,
			 GNM_CHAN_Y);

/* PS4 extension: mark a texture as video data.  Video planes are linear
 * (UNORM) and the NV12 chroma uses the RG8 U->G/V->A mapping; GUI textures
 * keep their SRGB/RGBA8 semantics.  Must match LinuxRendererGLES.cpp. */
#ifndef GL_TEXTURE_PS4_VIDEO_UNORM
#define GL_TEXTURE_PS4_VIDEO_UNORM 0x8F4F
#endif

extern void sceGnmFlushGarlic(void);
extern uint64_t sceKernelGetProcessTime(void);
/* Queue the DCB and the VideoOut flip in one call so the display flips as
 * soon as the GPU drains the command buffer, without a CPU spin on the EOP
 * label first.  Signature matches libSceGnmDriver and GoldHEN's frame_logger
 * hook. */
extern int sceGnmSubmitAndFlipCommandBuffers(uint32_t count, void* const dcbGpuAddrs[],
					     uint32_t* dcbSizesInBytes,
					     void* const ccbGpuAddrs[],
					     uint32_t* ccbSizesInBytes,
					     uint32_t videoOutHandle,
					     uint32_t displayBufferIndex,
					     uint32_t flipMode, int64_t flipArg);

typedef struct {
	int used;
	GLuint id;
	GLenum type;
	uint8_t* blob;
	size_t bloblen;
	int hassource;
} ShimShader;

typedef struct {
	char name[48];
	uint32_t off;
	uint8_t etype;
	uint8_t stage; /* 0 = VS, 1 = PS */
} ShimUniRef;

typedef struct {
	char name[48];
	GLint loc;
} ShimAttrRef;

typedef struct {
	int used, linked;
	GLuint id, vs_id, ps_id;
	GnmVsShader* vsh;
	GnmPsShader* psh;
	void* vsmem;
	void* psmem;
	void* vscode;
	void* pscode;
	void* fetchmem;
	uint32_t vs_fetch_slot, vs_vb_slot, vs_const_slot, ps_res_slot, ps_const_slot;
	int has_vs_const, has_ps_const, has_ps_res;
	uint8_t* cbuf_vs;
	uint8_t* cbuf_ps;
	size_t cbufsize;
	size_t vs_cbuf_size, ps_cbuf_size;
	ShimAttrRef attrs[MAX_ATTRS];
	int nattrs;
	ShimUniRef unis[MAX_UNIS];
	int nunis;
	ShimUniRef samplers[MAX_SAMPLERS];
	int sampler_unit[MAX_SAMPLERS];
	int nsamplers;
	int has_yuvmat;
	uint32_t yuvmat_off;
	uint8_t yuvmat_stage;
} ShimProgram;

typedef struct {
	int used;
	GLuint id;
	GLsizei width, height;
	GnmTexture tex;
	uint8_t* pixels;
	size_t bytes;
	int bpp; /* bytes per texel stored in `pixels` (1 for LUMINANCE/ALPHA) */
	GLint minfilter, magfilter, wraps, wrapt;
	int dynamic;
	/* Set by GL_TEXTURE_PS4_VIDEO_UNORM: video planes are linear and the
	 * NV12 chroma uses the U->G/V->A RG8 mapping. */
	int video_unorm;
} ShimTexture;

typedef struct {
	int used;
	GLuint id;
	void* data;
	size_t size;
} ShimBuffer;

/* Offscreen color target.  The attached texture and the GNM render target
 * share one allocation: rendering into the RT and sampling the texture must
 * see exactly the same tiling, otherwise the final video pass reads a
 * constant or shifted surface. */
#define MAX_FRAMEBUFFERS 16
typedef struct {
	int used;
	GLuint id;
	GLuint texid;
	GnmRenderTarget rt;
	int rt_valid;
	int width, height;
} ShimFramebuffer;

typedef struct {
	int enabled;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;
	size_t offset;
	GLuint buffer; /* GL_ARRAY_BUFFER bound when the pointer was set */
	int client;    /* pointer is a client array, not a buffer offset */
} ShimAttrib;

static ShimShader g_shaders[MAX_SHADERS];
static ShimProgram g_programs[MAX_PROGRAMS];

typedef struct {
	int valid;
	GLuint program;
	int fbidx;
	int colormask;
	GLint vp[4];
	GLint sc[4];
	int scissor_test;
	int blend;
	GLenum blend_src, blend_dst, blend_src_a, blend_dst_a;
	GLenum blend_equation, blend_equation_a;
	GnmCullMode cullmode;
	GnmFaceOrientation frontface;
	int depth_test, depth_mask;
	GLenum depth_func;
	GnmPrimitiveType prim;
	int indexed;
	GLenum index_type;
	unsigned draws;
} DrawCache;

typedef struct {
	int ready;
	MemoryAllocator mem;
	DisplayContext display;
	GnmRenderTarget fbtarget[FRAME_BUFFERS];
	GnmDepthRenderTarget depthtarget;
	int fbidx;
	GnmCommandBuffer cmd;
	void* cmdmem;
	uint64_t* label;
	uint64_t* gpu_start;
	uint64_t* gpu_end;
	uint32_t gpu_hz;
	GLuint program;
	/* state */
	GLfloat clear[4];
	GLfloat clear_depth;
	GLint vp[4];
	GLint sc[4];
	int scissor_test;
	int blend;
	GLenum blend_src, blend_dst;
	GLenum blend_src_a, blend_dst_a;
	GLenum blend_equation, blend_equation_a;
	int colormask;
	int depth_test, depth_mask;
	GLenum depth_func;
	int cull_face_enabled;
	GLenum cull_face, front_face;
	GLenum error;
	GLint unpack_row_length;
	DrawCache draw;
	/* objects */
	ShimTexture textures[MAX_TEXTURES];
	ShimBuffer buffers[MAX_BUFFERS];
	ShimFramebuffer framebuffers[MAX_FRAMEBUFFERS];
	GLuint cur_fbo; /* 0 = default backbuffer */
	ShimAttrib attribs[MAX_ATTRIBS];
	GLuint texunit[8];
	GLuint active_texture;
	GLuint array_buffer;
	GLuint element_buffer;
	/* per-frame arena + deferred frees */
	uint8_t* arena;
	size_t arenaoff;
	void* garbage[GARBAGE_MAX];
	int ngarbage;
	int textures_dirty;
	/* Set when a texture upload needs a GPU cache invalidate at the start
	 * of the next frame.  Frames that only draw static textures can skip
	 * the expensive CACHE_FLUSH_AND_INV event. */
	int cache_flush_needed;
	/* Defer the vsync wait of flip N to present N+1: the CPU builds the
	 * next frame while VideoOut waits for the display, instead of blocking
	 * right after submitting flip N. */
	int pipeline;
	int flip_pending;
	/* Wait for the next vblank before submitting the flip.  Off by default:
	 * the pipeline submits as soon as the previous flip retires, so the
	 * queued flip has a full refresh of margin for CPU scheduling jitter. */
	int vblank_lock;
	int flip_rate; /* OrbisFlipRate: 0 = 60 Hz, 1 = 30 Hz, 2 = 20 Hz */
	/* Use GNM's u16 USCALED fetch for Kodi's font UVs instead of expanding
	 * them to float on the CPU.  Opt-in until validated on the console. */
	int native_uv;
	/* PS4_GL_LA_RGBA8=1 or /data/kodi-ps4/la-rgba8 restores the old 4 B
	 * chroma path for A/B; default is the 2 B RG8 path. */
	int la_rgba8;
	unsigned present_count;
} GlState;

static GlState g;
static int g_font_dump_enabled;
/* Per-core sampling via sceKernelGetCpuUsageAll costs tens of milliseconds on
 * the render thread.  Keep it out of the frame loop unless explicitly enabled
 * for a diagnostic run. */
static int g_cpu_stats = -1;
static unsigned g_cpu_stats_windows;
/* Flip rate requested before displayctx_init() owns the VideoOut handle. */
static int g_pending_flip_rate = -1;
/* Perf diagnostic: time spent inside the GL->GNM translation, split from
 * the rest of Kodi's frame (which the Kodi-side PS4 frame profiler logs). */
static uint64_t g_perf_draw_us;
static uint64_t g_perf_tex_us;

typedef struct {
	GnmTexture texture;
	GnmSampler sampler;
} PsDescSetLike;

/* YV12 uses three samplers (Y/U/V); the descriptor set is an array of
 * texture+sampler pairs, so size it by the shader's sampler count instead
 * of hard-coding two. */
typedef PsDescSetLike PsDescSetPair;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Kodi exports WriteStartupTrace (C++ mangled) and keeps a canary fd open;
 * use it when present so the shim is visible even if /data file I/O is
 * restricted for the app sandbox.  The standalone test builds without
 * GLSHIM_KODI and gets a no-op. */
#if defined(GLSHIM_KODI)
extern void _Z17WriteStartupTracePKc(const char*) __attribute__((weak));
#endif

#if defined(GLSHIM_KODI)
/* Kodi's global constructors run before main and may leave the display in a
 * state where sceVideoOutOpen blocks.  Open the port as early as possible.
 * clang emits __attribute__((constructor(prio))) into .ctors.<prio>, which
 * the PS4 link script does not execute (only .init_array); register the
 * pointer explicitly so the loader runs it with the other constructors. */
static void shim_early_ctor(void) {
	glShimLog("ctor: early videoout");
	displayctx_early_open();
}
__attribute__((used, section(".init_array"))) static void (*const shim_early_ctor_entry)(
    void) = shim_early_ctor;
#endif

void PS4_EarlyVideoOutInit(void) {
	glShimLog("display: early init from main");
	displayctx_early_open();
}

#if defined(GLSHIM_KODI)
void glShimTrace(const char* stage) {
	if (_Z17WriteStartupTracePKc)
		_Z17WriteStartupTracePKc(stage);
}
#else
void glShimTrace(const char* stage) { (void)stage; }
#endif

void glShimLog(const char* fmt, ...) {
	static const char* kLogPath = "/data/kodi-ps4/glshim-shim.log";
	static const char* kOldLogPath = "/data/kodi-ps4/glshim-shim.old.log";
	static size_t log_bytes;
	static int session_started;
	static int header_written;
	static int log_fd = -1;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';

	/* One file per process/session: keep the previous log for the crash
	 * that is being investigated and make the 4 MiB cap effective per
	 * file instead of per process. */
	if (!session_started) {
		session_started = 1;
		if (log_fd >= 0) {
			close(log_fd);
			log_fd = -1;
		}
		rename(kLogPath, kOldLogPath);
		log_bytes = 0;
	}

	/* Keep the descriptor open: a per-line open/close serializes the render
	 * thread on the filesystem and shows up as a dropped frame every time the
	 * perf summary is written. */
	if (log_fd < 0)
		log_fd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (log_fd < 0)
		return;
	if (!header_written) {
		header_written = 1;
		char header[64];
		const int hn = snprintf(header, sizeof(header), "=== shim session t=%lld ===\n",
					(long long)time(NULL));
		if (hn > 0) {
			(void)write(log_fd, header, (size_t)hn);
			log_bytes += (size_t)hn;
		}
	}
	(void)write(log_fd, buf, (size_t)n);
	log_bytes += (size_t)n;
	if (log_bytes > 4u * 1024u * 1024u) {
		(void)ftruncate(log_fd, 0);
		log_bytes = 0;
	}
}

#define slog glShimLog

#if defined(PS4_RENDER_CANARY)
enum {
	CANARY_SLOT_CLEAR,
	CANARY_SLOT_DRAW,
	CANARY_SLOT_TEX,
	CANARY_SLOT_FBO,
	CANARY_SLOT_PROG,
	CANARY_SLOT_PRESENT,
	CANARY_SLOT_FLIP,
	CANARY_SLOT_SAMPLER,
	CANARY_SLOT_BINDTEX,
	CANARY_SLOT_COUNT
};
static unsigned g_canary_mask;
static unsigned g_canary_slots[CANARY_SLOT_COUNT];
static unsigned g_canary_magenta_frames;
static unsigned g_canary_clears;
#define CANARY_MARK(slot) (g_canary_mask |= (1u << (slot)))
#define CANARY_TAKE(slot, max) (g_canary_slots[slot]++ < (unsigned)(max))
#define CANARY_MASK() g_canary_mask
#else
#define CANARY_MARK(slot) do {} while (0)
#define CANARY_TAKE(slot, max) 0
#define CANARY_MASK() 0u
#endif

static void shim_set_error(GLenum e) {
	if (g.error == GL_NO_ERROR)
		g.error = e;
}

static void* arena_alloc(size_t size, size_t align) {
	if (!g.arena)
		return NULL;
	/* The per-frame arena feeds CPU vertex/uniform copies; keep it at the
	 * same 16-byte base alignment that malloc guarantees so compiler-
	 * generated SSE stores cannot fault with SIGBUS. */
	if (align < 16)
		align = 16;
	size_t off = (g.arenaoff + align - 1) & ~(align - 1);
	if (off + size > FRAME_ARENA_SIZE)
		return NULL;
	void* p = g.arena + off;
	g.arenaoff = off + size;
	memset(p, 0, size);
	return p;
}

static void garbage_add(void* p) {
	if (!p)
		return;
	if (g.ngarbage < GARBAGE_MAX) {
		g.garbage[g.ngarbage++] = p;
	} else {
		/* Freeing here could return memory the GPU is still reading.
		 * Leaking one allocation is safer than a use-after-free. */
		static int garbage_overflow_logged;
		if (!garbage_overflow_logged) {
			garbage_overflow_logged = 1;
			slog("garbage full; leaking %p", p);
		}
	}
}

static void garbage_drain(void) {
	for (int i = 0; i < g.ngarbage; i++)
		memalloc_free(&g.mem, g.garbage[i]);
	g.ngarbage = 0;
	g.arenaoff = 0;
}

/* ------------------------------------------------------------------ */
/* per-core CPU load                                                   */
/* ------------------------------------------------------------------ */

#define CPU_CORES 8
#define CPU_STATES 5 /* user, nice, sys, intr, idle */
#define CPU_STATE_IDLE 4

#ifndef SYS___sysctl
#define SYS___sysctl 202
#endif

/* libkernel exports sceKernelGetCpuUsageAll(), but the OpenOrbis headers do
 * not declare it.  The userland prototype returns one percentage per CPU in
 * per_core_pct and the number of entries in count. */
extern int sceKernelGetCpuUsageAll(int* per_core_pct, int* count);

static int g_cpu_source = -1; /* 0 sysctl, 1 procfs, 2 sceKernel */
static int g_cpu_cores;
static int g_cpu_prev_valid;
static uint64_t g_cpu_prev[CPU_CORES][CPU_STATES];
static uint64_t g_cpu_last_us;

/* Direct per-core percentages.  This avoids the cumulative-counter ABI of
 * sceKernelGetCpuUsage(), which is per thread and crashed when fed a large
 * Proc_Stats array. */
static int cpu_read_all_pct(int out[CPU_CORES]) {
	int pct[CPU_CORES] = {0};
	int count = 0;
	if (sceKernelGetCpuUsageAll(pct, &count) != 0 || count <= 0)
		return -1;
	if (count > CPU_CORES)
		count = CPU_CORES;
	for (int i = 0; i < count; i++) {
		int v = pct[i];
		if (v < 0)
			v = 0;
		if (v > 100)
			v = 100;
		out[i] = v;
	}
	g_cpu_cores = count;
	return 0;
}

static int cpu_read(uint64_t out[CPU_CORES][CPU_STATES]) {
	memset(out, 0, sizeof(uint64_t) * CPU_CORES * CPU_STATES);

	/* FreeBSD kern.cp_times: long[CPUSTATES] per CPU. */
	int mib[2] = {1 /* CTL_KERN */, 39 /* KERN_CP_TIMES */};
	long buf[CPU_CORES * CPU_STATES];
	size_t len = sizeof(buf);
	if (syscall(SYS___sysctl, mib, 2, buf, &len, NULL, 0) == 0 &&
	    len >= (size_t)(CPU_STATES * sizeof(long))) {
		int n = (int)(len / (CPU_STATES * sizeof(long)));
		if (n > CPU_CORES)
			n = CPU_CORES;
		for (int i = 0; i < n; i++)
			for (int j = 0; j < CPU_STATES; j++)
				out[i][j] = (uint64_t)buf[i * CPU_STATES + j];
		g_cpu_cores = n;
		return 0;
	}

	/* procfs fallback. */
	FILE* f = fopen("/proc/stat", "r");
	if (f) {
		char line[256];
		int n = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
				continue;
			const int id = atoi(line + 3);
			if (id < 0 || id >= CPU_CORES)
				continue;
			unsigned long long v[CPU_STATES] = {0};
			if (sscanf(line + 3, "%*d %llu %llu %llu %llu %llu", &v[0], &v[1],
				   &v[2], &v[3], &v[4]) == 5) {
				for (int j = 0; j < CPU_STATES; j++)
					out[id][j] = (uint64_t)v[j];
				n++;
			}
		}
		fclose(f);
		if (n > 0) {
			g_cpu_cores = n;
			return 1;
		}
	}

	/* Last resort: per-thread usage; lo_data is treated as the CPU id. */
	static struct Proc_Stats stats[512];
	int32_t size = 512;
	if (sceKernelGetCpuUsage(stats, &size) == 0 && size > 0) {
		int n = 0;
		for (int i = 0; i < size && i < 512; i++) {
			const int cpu = stats[i].lo_data;
			if (cpu < 0 || cpu >= CPU_CORES)
				continue;
			const uint64_t user =
			    (uint64_t)stats[i].user_cpu_usage_time.tv_sec * 1000000000ull +
			    (uint64_t)stats[i].user_cpu_usage_time.tv_nsec;
			const uint64_t sys =
			    (uint64_t)stats[i].system_cpu_usage_time.tv_sec * 1000000000ull +
			    (uint64_t)stats[i].system_cpu_usage_time.tv_nsec;
			out[cpu][0] += user;
			out[cpu][2] += sys;
			n = CPU_CORES;
		}
		if (n > 0) {
			g_cpu_cores = n;
			return 2;
		}
	}
	return -1;
}

static void cpu_log_cores(void) {
	int pct[CPU_CORES];
	if (cpu_read_all_pct(pct) == 0) {
		static int logged;
		if (!logged) {
			slog("cpu: source=sceKernelGetCpuUsageAll cores=%d", g_cpu_cores);
			logged = 1;
		}
		char line[256];
		int off = 0;
		for (int i = 0; i < g_cpu_cores; i++) {
			off += snprintf(line + off, sizeof(line) - (size_t)off, " c%d %d", i,
					pct[i]);
			if (off >= (int)sizeof(line) - 16)
				break;
		}
		slog("cpu:%s %%", line);
		return;
	}

	uint64_t cur[CPU_CORES][CPU_STATES];
	const int src = cpu_read(cur);
	if (src < 0) {
		static int logged;
		if (!logged) {
			slog("cpu: per-core load unavailable");
			logged = 1;
		}
		return;
	}
	if (g_cpu_source < 0) {
		g_cpu_source = src;
		slog("cpu: source=%s cores=%d",
		     src == 0 ? "sysctl" : (src == 1 ? "procfs" : "sceKernel"),
		     g_cpu_cores);
	}
	const uint64_t now_us = sceKernelGetProcessTime();
	if (!g_cpu_prev_valid) {
		memcpy(g_cpu_prev, cur, sizeof(cur));
		g_cpu_prev_valid = 1;
		g_cpu_last_us = now_us;
		return;
	}
	const uint64_t wall_us = now_us - g_cpu_last_us;
	char line[256];
	int off = 0;
	for (int i = 0; i < g_cpu_cores; i++) {
		uint64_t total = 0, idle = 0;
		for (int j = 0; j < CPU_STATES; j++) {
			const uint64_t d = cur[i][j] - g_cpu_prev[i][j];
			total += d;
			if (j == CPU_STATE_IDLE)
				idle += d;
		}
		double pct = 0.0;
		if (src == 2)
			pct = wall_us ? 100.0 * (double)total / ((double)wall_us * 1000.0) : 0.0;
		else
			pct = total ? 100.0 * (double)(total - idle) / (double)total : 0.0;
		if (pct > 100.0)
			pct = 100.0;
		off += snprintf(line + off, sizeof(line) - (size_t)off, " c%d %.1f", i, pct);
		if (off >= (int)sizeof(line) - 16)
			break;
	}
	slog("cpu:%s %%", line);
	memcpy(g_cpu_prev, cur, sizeof(cur));
	g_cpu_last_us = now_us;
}

static ShimTexture* texture_by_id(GLuint id) {
	if (id == 0 || id > MAX_TEXTURES)
		return NULL;
	ShimTexture* t = &g.textures[id - 1];
	return t->used ? t : NULL;
}

static ShimTexture* current_texture(void) {
	GLuint unit = g.active_texture < 8 ? g.active_texture : 0;
	return texture_by_id(g.texunit[unit]);
}

/* The allocation is `bytes + 8`; the last eight bytes are a canary that no
 * legitimate texture upload touches.  Check it when the texture is replaced or
 * freed, and after glTexSubImage2D writes. */
static void texture_canary_write(ShimTexture* t) {
	if (!t || !t->pixels)
		return;
	const uint64_t magic = SHIM_TEXTURE_CANARY;
	memcpy(t->pixels + t->bytes, &magic, sizeof(magic));
}

static int texture_canary_logs;

static int texture_canary_check(const ShimTexture* t, const char* where) {
	if (!t || !t->pixels)
		return 1;
	uint64_t magic = 0;
	memcpy(&magic, t->pixels + t->bytes, sizeof(magic));
	if (magic != SHIM_TEXTURE_CANARY) {
		if (texture_canary_logs++ < 8)
			slog("tex: canary corrupt where=%s id=%u bytes=%zu got=0x%llx", where,
			     t->id, t->bytes, (unsigned long long)magic);
		return 0;
	}
	return 1;
}

static ShimFramebuffer* framebuffer_by_id(GLuint id) {
	if (id == 0 || id > MAX_FRAMEBUFFERS)
		return NULL;
	ShimFramebuffer* fb = &g.framebuffers[id - 1];
	return fb->used ? fb : NULL;
}

/* Current color target: an attached offscreen FBO when one is bound,
 * otherwise the scanned-out backbuffer.  NULL means the bound FBO is
 * incomplete, in which case draws must be dropped (GL semantics). */
static GnmRenderTarget* current_render_target(void) {
	ShimFramebuffer* fb = framebuffer_by_id(g.cur_fbo);
	if (fb)
		return fb->rt_valid ? &fb->rt : NULL;
	return &g.fbtarget[g.fbidx];
}

static ShimBuffer* buffer_by_id(GLuint id) {
	if (id == 0 || id > MAX_BUFFERS)
		return NULL;
	ShimBuffer* b = &g.buffers[id - 1];
	return b->used ? b : NULL;
}

static ShimShader* shader_by_id(GLuint id) {
	for (int i = 0; i < MAX_SHADERS; i++)
		if (g_shaders[i].used && g_shaders[i].id == id)
			return &g_shaders[i];
	return NULL;
}

static ShimProgram* program_by_id(GLuint id) {
	if (!id)
		return NULL;
	for (int i = 0; i < MAX_PROGRAMS; i++)
		if (g_programs[i].used && g_programs[i].id == id)
			return &g_programs[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* engine lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void begin_gpu_frame(void);

int glShimInit(unsigned width, unsigned height) {
	const uint64_t init_t0 = sceKernelGetProcessTime();
	glShimLog("init: enter");
	glShimTrace("shim-init-enter");
	if (g.ready)
		return 1;
	memset(&g, 0, sizeof(g));
	/* Atlas dumps are a diagnostic tool: 10 files of up to 1 MB each add
	 * noticeable I/O to startup/transitions.  Only enable them explicitly
	 * with PS4_FONT_DUMP=1; normal runs must not write 5+ MB during the
	 * first window loads. */
	g_font_dump_enabled = getenv("PS4_FONT_DUMP") != NULL;
	/* Present mode: the deferred-vsync pipeline keeps one flip per vsync
	 * and lets the CPU build frame N+1 while the display scans N.  It is
	 * the default because the synchronous path lets the close-animation
	 * loop run at 300+ iterations/s and makes transitions uneven.
	 * PS4_GL_SYNC_PRESENT=1 (or "sync" in
	 * /data/kodi-ps4/present-mode.txt) restores the old blocking path
	 * without rebuilding; PS4_GL_ASYNC_PRESENT is kept as an alias. */
	g.pipeline = 1;
	{
		const char* sync_env = getenv("PS4_GL_SYNC_PRESENT");
		const char* async_env = getenv("PS4_GL_ASYNC_PRESENT");
		if (sync_env && sync_env[0] == '1')
			g.pipeline = 0;
		else if (async_env && async_env[0] == '1')
			g.pipeline = 1;
		else {
			FILE* f = fopen("/data/kodi-ps4/present-mode.txt", "r");
			if (f) {
				char mode[32] = {0};
				if (fgets(mode, sizeof(mode), f)) {
					if (strstr(mode, "sync"))
						g.pipeline = 0;
					else if (strstr(mode, "async"))
						g.pipeline = 1;
				}
				fclose(f);
			}
		}
	}
	g.native_uv = getenv("PS4_GL_NATIVE_UV") != NULL;
	g.la_rgba8 = getenv("PS4_GL_LA_RGBA8") != NULL;
	if (!g.la_rgba8) {
		FILE* f = fopen("/data/kodi-ps4/la-rgba8", "r");
		if (f) {
			g.la_rgba8 = 1;
			fclose(f);
		}
	}
	g.vblank_lock = 0;
	{
		const char* lock_env = getenv("PS4_GL_VBLANK_LOCK");
		if (lock_env && lock_env[0] == '1')
			g.vblank_lock = 1;
		else
		{
			FILE* f = fopen("/data/kodi-ps4/present-vblank-lock", "r");
			if (f)
			{
				char mode[16] = {0};
				if (fgets(mode, sizeof(mode), f) && strstr(mode, "1"))
					g.vblank_lock = 1;
				fclose(f);
			}
		}
	}
	g.flip_pending = 0;
	g.present_count = 0;
#define INIT_MS() ((double)(sceKernelGetProcessTime() - init_t0) / 1000.0)
	g.vp[0] = 0;
	g.vp[1] = 0;
	g.vp[2] = (GLint)width;
	g.vp[3] = (GLint)height;
	g.sc[0] = 0;
	g.sc[1] = 0;
	g.sc[2] = (GLint)width;
	g.sc[3] = (GLint)height;
	g.scissor_test = 1;
	g.clear[0] = g.clear[1] = g.clear[2] = 0.0f;
	g.clear[3] = 1.0f;
	g.clear_depth = 1.0f;
	g.blend = 1;
	g.blend_src = GL_SRC_ALPHA;
	g.blend_dst = GL_ONE_MINUS_SRC_ALPHA;
	g.blend_src_a = GL_ONE;
	g.blend_dst_a = GL_ONE_MINUS_SRC_ALPHA;
	g.blend_equation = GL_FUNC_ADD;
	g.blend_equation_a = GL_FUNC_ADD;
	g.colormask = 0xf;
	g.depth_mask = 1;
	g.depth_func = GL_LESS;
	g.cull_face = GL_BACK;
	g.front_face = GL_CCW;

	slog("init: begin (pipeline+perf21-vblanklock)");
	g.mem = memalloc_init(64 * 1024 * 1024,
			      ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
				  ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
			      ORBIS_KERNEL_WC_GARLIC);
	slog("init: memalloc ok %.2f ms", INIT_MS());
	glShimTrace("shim-init-memalloc-ok");
	if (!initclearutilitymem(&g.mem, k_clear_frag_sb, k_clear_frag_sb_len)) {
		slog("init: clear utility failed");
		return 0;
	}
	slog("init: clear utility ok %.2f ms", INIT_MS());
	glShimTrace("shim-init-clear-ok");
	if (!displayctx_init(&g.display)) {
		slog("init: display failed");
		return 0;
	}
	slog("init: display ok %ux%u %.2f ms", g.display.screenw, g.display.screenh,
	     INIT_MS());
	glShimTrace("shim-init-display-ok");
	for (int i = 0; i < FRAME_BUFFERS; i++) {
		if (!initcolortarget(&g.fbtarget[i], &g.mem, g.display.screenw,
				     g.display.screenh, GNM_FMT_R8G8B8A8_SRGB,
				     gnmGpuMode())) {
			slog("init: color target %d failed", i);
			return 0;
		}
	}
	g.fbidx = 0;
	slog("init: color targets ok (%d) %.2f ms", FRAME_BUFFERS, INIT_MS());
	glShimTrace("shim-init-colortarget-ok");
	if (!initdepthtarget(&g.depthtarget, &g.mem, g.display.screenw, g.display.screenh, 0,
			     1, 1, GNM_Z_24, GNM_STENCIL_INVALID, gnmGpuMode())) {
		slog("init: depth target failed");
		return 0;
	}
	slog("init: depth target ok %.2f ms", INIT_MS());
	if (!displayctx_setrts(&g.display, g.fbtarget, FRAME_BUFFERS)) {
		slog("init: set render targets failed");
		return 0;
	}
	slog("init: render targets ok %.2f ms", INIT_MS());
	glShimTrace("shim-init-rts-ok");
	g.cmdmem = memalloc_alloc(&g.mem, GNM_INDIRECT_BUFFER_MAX_BYTESIZE,
				  GNM_ALIGNMENT_BUFFER_BYTES);
	if (!g.cmdmem)
		return 0;
	g.cmd = gnmCmdInit(g.cmdmem, GNM_INDIRECT_BUFFER_MAX_BYTESIZE, NULL, NULL);
	g.label = memalloc_alloc(&g.mem, 3 * sizeof(uint64_t), sizeof(uint64_t));
	g.arena = memalloc_alloc(&g.mem, FRAME_ARENA_SIZE, GNM_ALIGNMENT_BUFFER_BYTES);
	if (!g.label || !g.arena)
		return 0;
	g.gpu_start = g.label + 1;
	g.gpu_end = g.label + 2;
	*g.label = 1;
	*g.gpu_start = 0;
	*g.gpu_end = 0;
	/* The OpenOrbis import stubs do not export the GPU clock query, so use
	 * the base PS4 GPU clock for the diagnostic conversion.  The GPU clock
	 * event itself is exact; only the Hz-to-ms conversion is nominal. */
	g.gpu_hz = 800000000u;
	slog("init: gpu clock %u Hz (nominal), cpu clock %d Hz",
	     g.gpu_hz, sceKernelGetCpuFrequency());
	begin_gpu_frame();
	g.ready = 1;
#if defined(PS4_RENDER_CANARY)
	slog("canary: init ready");
#endif
	if (g_pending_flip_rate >= 0) {
		const int rate = g_pending_flip_rate;
		g_pending_flip_rate = -1;
		glShimSetFlipRate(rate);
	}
	cpu_log_cores();
	slog("init: present %s, vblank lock %s, native uv %s",
	     g.pipeline ? "pipeline" : "sync", g.vblank_lock ? "on" : "off",
	     g.native_uv ? "on" : "off");
	slog("init: ok %ux%u %.2f ms", g.display.screenw, g.display.screenh, INIT_MS());
	glShimTrace("shim-init-ok");
	return 1;
}
#undef INIT_MS

void glShimShutdown(void) {
	if (g.ready) {
		if (g.flip_pending) {
			(void)displayctx_wait_flip_status(&g.display);
			g.flip_pending = 0;
		}
		displayctx_destroy(&g.display);
		memalloc_destroy(&g.mem);
	}
	memset(&g, 0, sizeof(g));
}

/* Start a new command buffer and timestamp the GPU clock at its beginning.
 * The matching timestamp is written just before the present EOP event, so
 * the difference is the time the GPU actually spent on this frame. */
static void begin_gpu_frame(void) {
	gnmCmdReset(&g.cmd);
	if (g.gpu_start) {
		/* Font atlases are uploaded by the CPU after the previous frame,
		 * and the GPU L2 can otherwise keep sampling stale texels even
		 * when the descriptor points at a fresh block.  Only invalidate
		 * when a texture actually changed; a static menu frame can use
		 * the cheaper timestamp event. */
		if (g.cache_flush_needed) {
			gnmDrawCmdEventWriteEop(&g.cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT,
						(uint64_t)g.gpu_start,
						GNM_DATA_SEL_SEND_GPU_CLOCK, 0);
			g.cache_flush_needed = 0;
		} else {
			gnmDrawCmdEventWriteEop(&g.cmd, GNM_CACHE_FLUSH_TS,
						(uint64_t)g.gpu_start,
						GNM_DATA_SEL_SEND_GPU_CLOCK, 0);
		}
	}
}

uint64_t glShimPresentCount(void) {
	return (uint64_t)g.present_count;
}

uint64_t glShimGetProcessTimeUs(void) {
	return sceKernelGetProcessTime();
}

void glShimWaitFlip(void) {
	/* The next glShimPresent() waits for the flip that is still pending.
	 * Waiting here as well would consume two vblanks per close-animation
	 * iteration (30 fps).  Keep the pending flag set so the present path
	 * does the single wait. */
}

/* VideoOut device capability.  Bit 0x80 is BT2020/PQ (HDR10). */
typedef struct {
	uint64_t capability;
} ShimVideoOutCapability;

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t paneWidth;
	uint32_t paneHeight;
	uint64_t refreshRate;
	float screenSize;
	uint16_t flags;
	uint16_t reserved0;
	uint32_t reserved1[3];
} ShimVideoOutResolutionStatus;

typedef struct {
	uint64_t num;
	uint64_t ptime;
	uint64_t stime;
	int64_t flipArg;
	uint64_t reserved[2];
	int32_t numGpuFlipPending;
	int32_t numFlipPending;
	int32_t currentBuffer;
	uint32_t reserved1;
} ShimVideoOutFlipStatus;

extern int sceVideoOutGetDeviceCapabilityInfo_(int handle, ShimVideoOutCapability* info);
extern int sceVideoOutGetResolutionStatus(int handle, ShimVideoOutResolutionStatus* status);
extern int sceVideoOutGetFlipStatus(int handle, ShimVideoOutFlipStatus* status);
extern int sceVideoOutIsFlipPending(int handle);

int glShimGetVideoOutHandle(void) {
	return g.ready ? g.display.videohandle : -1;
}

uint64_t glShimGetVideoOutCapability(void) {
	if (!g.ready || g.display.videohandle <= 0)
		return 0;
	ShimVideoOutCapability info = {0};
	const int rc = sceVideoOutGetDeviceCapabilityInfo_(g.display.videohandle, &info);
	slog("hdr: GetDeviceCapabilityInfo rc=0x%x capability=0x%llx HDR10=%d", rc,
	     (unsigned long long)info.capability, (info.capability & 0x80) ? 1 : 0);
	return rc == 0 ? info.capability : 0;
}

/* Number of flips VideoOut has actually scanned out.  This is the true
 * display rate, independent of how many frames the render loop queued. */
uint64_t glShimGetFlipCount(void) {
	if (!g.ready || g.display.videohandle <= 0)
		return 0;
	ShimVideoOutFlipStatus st = {0};
	if (sceVideoOutGetFlipStatus(g.display.videohandle, &st) != 0)
		return 0;
	return st.num;
}

void glShimSetFlipRate(int rate) {
	if (rate < 0)
		rate = 0;
	if (rate > 2)
		rate = 2;
	if (!g.ready || g.display.videohandle <= 0) {
		/* SetFullScreen can run before the EGL surface exists.  Remember
		 * the mode and apply it once displayctx_init() completes. */
		g_pending_flip_rate = rate;
		return;
	}
	if (g.flip_rate == rate)
		return;
	if (!displayctx_set_flip_rate(&g.display, rate)) {
		slog("display: flip rate %d failed", rate);
		return;
	}
	g.flip_rate = rate;
	slog("display: flip rate %d (%s)", rate,
	     rate == 0 ? "60Hz" : (rate == 1 ? "30Hz" : "20Hz"));
}

void glShimPresent(void) {
	if (!g.ready)
		glShimInit(1920, 1080);
	if (!g.ready)
		return;
	static int combined_flip = -1;
	if (combined_flip < 0) {
		/* Default on: the DCB and the flip go out in one call, which is how
		 * PS4 games present.  PS4_GL_COMBINED_FLIP=0 restores the old split
		 * submit + flip path for comparison. */
		const char* v = getenv("PS4_GL_COMBINED_FLIP");
		combined_flip = !(v && v[0] == '0');
		slog("present: %s submit+flip", combined_flip ? "combined" : "split");
	}
	const uint64_t present_start = sceKernelGetProcessTime();
#if defined(PS4_RENDER_CANARY)
	if (g_canary_magenta_frames < 4) {
		GnmRenderTarget* mrt = current_render_target();
		if (mrt) {
			float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
			clearcolortarget(&g.cmd, mrt, magenta);
		}
		if (g_canary_magenta_frames == 0)
			slog("canary: magenta fill frames=4");
	}
	g_canary_magenta_frames++;
#endif
	/* GPU clock at the end of all draws; the start clock was written when
	 * this command buffer was opened. */
	gnmDrawCmdEventWriteEop(&g.cmd, GNM_CACHE_FLUSH_TS, (uint64_t)g.gpu_end,
				GNM_DATA_SEL_SEND_GPU_CLOCK, 0);
	gnmDrawCmdEventWriteEop(&g.cmd, GNM_CACHE_FLUSH_TS, (uint64_t)g.label,
				GNM_DATA_SEL_SEND_DATA64, 1);
	/* Font atlases are uploaded with glTexSubImage2D after the texture was
	 * first used.  Invalidate the GPU texture cache so later glyph rows are
	 * visible, otherwise the atlas keeps sampling stale/zero rows. */
	if (g.textures_dirty) {
		sceGnmFlushGarlic();
		g.textures_dirty = 0;
	}
	void* dcbaddr = (void*)g.cmd.beginptr;
	uint32_t dcbsize = (uint32_t)(g.cmd.cmdptr - g.cmd.beginptr) * sizeof(uint32_t);
	/* The EOP event writes 1 when the GPU has consumed every draw in this
	 * command buffer.  Reset the label *before* submitting: the old code left
	 * it at 1, so the wait below returned immediately and garbage_drain()
	 * freed vertex/texture memory while the GPU was still reading it.  That
	 * caused the font atlas corruption and the half-drawn transitions. */
	*g.label = 0;
	const uint64_t submit_start = sceKernelGetProcessTime();
	bool combined_ok = false;
	/* Combined mode only makes sense with the synchronous present: the
	 * flip is part of the submit, so we cannot defer its wait. */
	if (combined_flip && !g.pipeline) {
		if (sceGnmSubmitAndFlipCommandBuffers(
			1, &dcbaddr, &dcbsize, NULL, NULL,
			(uint32_t)g.display.videohandle, (uint32_t)g.fbidx,
			/* ORBIS_VIDEO_OUT_FLIP_VSYNC */
			1, 0) == 0) {
			combined_ok = true;
		} else {
			slog("present: combined submit+flip failed, using split path");
			combined_flip = 0;
		}
	}
	if (!combined_ok && sceGnmSubmitCommandBuffers(1, &dcbaddr, &dcbsize, NULL, 0) != 0) {
		slog("present: SubmitCommandBuffers failed");
		shim_set_error(GL_INVALID_OPERATION);
		*g.label = 1;
		begin_gpu_frame();
		g.draw.valid = 0;
		return;
	}
	const uint64_t submit_us = sceKernelGetProcessTime() - submit_start;
	uint64_t wait_us = 0;
	uint64_t prevflip_us = 0;
	uint64_t flip_submit_us = 0;
	bool flip_ok = true;
	if (combined_ok) {
		/* The flip is queued behind the draws, so waiting for the flip
		 * event proves the GPU finished and the buffer was scanned out.
		 * This is the same guarantee the EOP label gave us, without the
		 * extra CPU spin. */
		const uint64_t wait_start = sceKernelGetProcessTime();
		flip_ok = displayctx_wait_flip(&g.display);
		wait_us = sceKernelGetProcessTime() - wait_start;
	} else {
		const uint64_t wait_start = sceKernelGetProcessTime();
		uint64_t wait_deadline = wait_start + 5ull * 1000ull * 1000ull;
		while (*g.label != 1) {
			if (sceKernelGetProcessTime() > wait_deadline) {
				slog("present: GPU wait timeout (label=%llu)",
				     (unsigned long long)*g.label);
				break;
			}
		}
		wait_us = sceKernelGetProcessTime() - wait_start;
		/* Pipeline mode: wait for the *previous* flip before queueing this one.
		 * The CPU spent the previous vsync interval building this frame, so the
		 * wait is normally already satisfied; the display stays at one flip per
		 * vsync and CPU spikes no longer extend the frame period. */
		const uint64_t prevflip_start = sceKernelGetProcessTime();
		if (g.pipeline && g.flip_pending) {
			const bool flip_done =
			    g.vblank_lock ? displayctx_wait_flip(&g.display)
					  : displayctx_wait_flip_status(&g.display);
			if (!flip_done) {
				slog("present: previous flip wait failed");
				shim_set_error(GL_INVALID_OPERATION);
			}
			g.flip_pending = 0;
		}
		prevflip_us = sceKernelGetProcessTime() - prevflip_start;
		const uint64_t flip_start = sceKernelGetProcessTime();
		if (g.pipeline) {
			flip_ok = displayctx_submit_flip(&g.display, (unsigned)g.fbidx);
			if (flip_ok) {
				g.flip_pending = 1;
			} else {
				/* Do not leave the display without a frame if the queue
				 * rejects the async submit: fall back to the blocking flip. */
				slog("present: pipeline submit failed, switching to sync");
				g.pipeline = 0;
				g.flip_pending = 0;
				flip_ok = displayctx_flip(&g.display, (unsigned)g.fbidx);
			}
		} else {
			flip_ok = displayctx_flip(&g.display, (unsigned)g.fbidx);
		}
		flip_submit_us = sceKernelGetProcessTime() - flip_start;
	}
	const uint64_t flip_us = prevflip_us + flip_submit_us;
	if (!flip_ok) {
		slog("present: flip failed");
		shim_set_error(GL_INVALID_OPERATION);
	}
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_PRESENT);
	if (flip_ok)
		CANARY_MARK(CANARY_SLOT_FLIP);
	{
		static unsigned canary_presents;
		if (canary_presents < 12 || (canary_presents % 60) == 0)
			slog("canary: present frame=%u fbidx=%d draws=%u clears=%u mask=0x%x flip=%d combined=%d handle=%d fbo=%u prog=%u",
			     canary_presents, g.fbidx, g.draw.draws, g_canary_clears,
			     CANARY_MASK(), flip_ok, combined_ok, g.display.videohandle,
			     g.cur_fbo, g.program);
		g_canary_clears = 0;
		canary_presents++;
	}
#endif
	g.fbidx = (g.fbidx + 1) % FRAME_BUFFERS;
	sceGnmSubmitDone();
	begin_gpu_frame();
	g.draw.valid = 0;
	garbage_drain();

	static uint64_t perf_accum_cpu_us;
	static uint64_t perf_accum_present_us;
	static uint64_t perf_accum_submit_us;
	static uint64_t perf_accum_wait_us;
	static uint64_t perf_accum_prevflip_us;
	static uint64_t perf_accum_flipsub_us;
	static uint64_t perf_accum_flip_us;
	static uint64_t perf_accum_gpu_cycles;
	static uint64_t last_present_end;
	static uint64_t perf_max_frame_us;
	static unsigned perf_frames;
	static unsigned perf_draws;
	static uint64_t last_flip_count;
	static uint64_t last_perf_end_us;
	const uint64_t present_end = sceKernelGetProcessTime();
	const uint64_t present_us = present_end - present_start;
	if (g.present_count < 5) {
		slog("present[%u]: submit %.3f wait %.3f prevflip %.3f flip %.3f total %.3f ms",
		     g.present_count, submit_us / 1000.0, wait_us / 1000.0,
		     prevflip_us / 1000.0, flip_submit_us / 1000.0,
		     present_us / 1000.0);
	}
	g.present_count++;
	const uint64_t cpu_us = last_present_end ? present_start - last_present_end : 0;
	const uint64_t gpu_cycles =
	    (*g.gpu_end > *g.gpu_start) ? (*g.gpu_end - *g.gpu_start) : 0;
	last_present_end = present_end;
	const uint64_t frame_us = cpu_us + present_us;
	if (frame_us > perf_max_frame_us)
		perf_max_frame_us = frame_us;
	perf_accum_cpu_us += cpu_us;
	perf_accum_present_us += present_us;
	perf_accum_submit_us += submit_us;
	perf_accum_wait_us += wait_us;
	perf_accum_prevflip_us += prevflip_us;
	perf_accum_flipsub_us += flip_submit_us;
	perf_accum_flip_us += flip_us;
	perf_accum_gpu_cycles += gpu_cycles;
	perf_draws += g.draw.draws;
	g.draw.draws = 0;
	perf_frames++;
	if (perf_frames >= 60) {
		const double cpu_ms = (double)perf_accum_cpu_us / 1000.0 / (double)perf_frames;
		const double present_ms =
		    (double)perf_accum_present_us / 1000.0 / (double)perf_frames;
		const double submit_ms =
		    (double)perf_accum_submit_us / 1000.0 / (double)perf_frames;
		const double wait_ms = (double)perf_accum_wait_us / 1000.0 / (double)perf_frames;
		const double prevflip_ms =
		    (double)perf_accum_prevflip_us / 1000.0 / (double)perf_frames;
		const double flipsub_ms =
		    (double)perf_accum_flipsub_us / 1000.0 / (double)perf_frames;
		const double flip_ms =
		    (double)perf_accum_flip_us / 1000.0 / (double)perf_frames;
		const double gpu_ms =
		    g.gpu_hz ? (double)perf_accum_gpu_cycles * 1000.0 /
				   (double)g.gpu_hz / (double)perf_frames
			     : 0.0;
		const double frame_ms = cpu_ms + present_ms;
		const double draw_ms =
		    (double)g_perf_draw_us / 1000.0 / (double)perf_frames;
		const double tex_ms =
		    (double)g_perf_tex_us / 1000.0 / (double)perf_frames;

		/* Process CPU time via getrusage: reliable and already used by
		 * Kodi on POSIX.  It is sampled once per perf window. */
		static uint64_t last_rusage_us;
		static uint64_t last_rusage_wall_us;
		static int rusage_state; /* 0 unknown, 1 ok, -1 failed */
		double proc_cpu_pct = 0.0;
		if (rusage_state >= 0) {
			struct rusage ru;
			if (getrusage(RUSAGE_SELF, &ru) == 0) {
				const uint64_t total_us =
				    ((uint64_t)ru.ru_utime.tv_sec +
				     (uint64_t)ru.ru_stime.tv_sec) *
					1000000ull +
				    (uint64_t)ru.ru_utime.tv_usec +
				    (uint64_t)ru.ru_stime.tv_usec;
				if (last_rusage_wall_us) {
					const uint64_t wall = present_end - last_rusage_wall_us;
					const uint64_t used = total_us - last_rusage_us;
					if (wall && used <= wall * 16ull)
						proc_cpu_pct =
						    100.0 * (double)used / (double)wall;
				}
				last_rusage_wall_us = present_end;
				last_rusage_us = total_us;
				rusage_state = 1;
			} else {
				if (rusage_state == 0)
					slog("perf: getrusage failed");
				rusage_state = -1;
			}
		}

		struct timespec wall;
		clock_gettime(CLOCK_REALTIME, &wall);
		const int flip_pending = sceVideoOutIsFlipPending(g.display.videohandle);
		/* Actual scanout rate, independent of how many frames the render
		 * loop queued.  If this is 30 while the frame loop says 60, the
		 * display is only consuming every other flip. */
		const uint64_t flip_count = glShimGetFlipCount();
		const uint64_t flip_delta = flip_count - last_flip_count;
		const uint64_t perf_window_us =
		    last_perf_end_us ? present_end - last_perf_end_us : 0;
		last_flip_count = flip_count;
		last_perf_end_us = present_end;
		const double flip_fps =
		    perf_window_us ? (double)flip_delta * 1000000.0 / (double)perf_window_us
				   : 0.0;
		uint64_t flex_bytes = 0;
		typedef int32_t (*AvailableFlexibleFn)(uint64_t*);
		AvailableFlexibleFn availableFlexible =
		    (AvailableFlexibleFn)(void*)&sceKernelAvailableFlexibleMemorySize;
		if (availableFlexible(&flex_bytes) != 0)
			flex_bytes = 0;
		size_t tex_bytes = 0;
		for (int ti = 0; ti < MAX_TEXTURES; ti++)
			if (g.textures[ti].used)
				tex_bytes += g.textures[ti].bytes;
		slog("perf: t=%lld.%03ld %u frames cpu %.2f present %.2f (submit %.2f wait %.2f "
		     "prev %.2f flipsub %.2f flip %.2f) "
		     "gpu %.2f frame %.2f max_frame %.2f ms (%.1f fps) draws/frame %.1f "
		     "draw %.2f tex %.2f ms proc-cpu %.1f%% pending %d flips %llu (%.1f fps) "
		     "flex=%lluMB tex=%zuKB gmem_used=%zuKB gmem_free=%zuKB gmem_total=%zuKB "
		     "gmem_blocks=%zu",
		     (long long)wall.tv_sec, wall.tv_nsec / 1000000,
		     perf_frames, cpu_ms, present_ms, submit_ms, wait_ms, prevflip_ms,
		     flipsub_ms, flip_ms, gpu_ms, frame_ms,
		     (double)perf_max_frame_us / 1000.0,
		     frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0,
		     (double)perf_draws / (double)perf_frames, draw_ms, tex_ms,
		     proc_cpu_pct, flip_pending, (unsigned long long)flip_delta, flip_fps,
		     (unsigned long long)(flex_bytes / (1024 * 1024)), tex_bytes / 1024,
		     memalloc_used_bytes(&g.mem) / 1024,
		     memalloc_free_bytes(&g.mem) / 1024,
		     memalloc_total_bytes(&g.mem) / 1024,
		     g.mem.mblocks.numelements);
		if (g_cpu_stats < 0)
			g_cpu_stats = (getenv("PS4_GL_CPU_STATS") != NULL ||
				       access("/data/kodi-ps4/glshim-cpu-stats", F_OK) == 0);
		if (g_cpu_stats && ++g_cpu_stats_windows >= 5) {
			g_cpu_stats_windows = 0;
			cpu_log_cores();
		}
		perf_accum_cpu_us = 0;
		perf_accum_present_us = 0;
		perf_accum_submit_us = 0;
		perf_accum_wait_us = 0;
		perf_accum_prevflip_us = 0;
		perf_accum_flipsub_us = 0;
		perf_accum_flip_us = 0;
		perf_accum_gpu_cycles = 0;
		perf_max_frame_us = 0;
		perf_frames = 0;
		perf_draws = 0;
		g_perf_draw_us = 0;
		g_perf_tex_us = 0;
	}
}

/* ------------------------------------------------------------------ */
/* GL state                                                            */
/* ------------------------------------------------------------------ */

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
	g.vp[0] = x;
	g.vp[1] = y;
	g.vp[2] = w;
	g.vp[3] = h;
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
	g.sc[0] = x;
	g.sc[1] = y;
	g.sc[2] = w;
	g.sc[3] = h;
}
void glEnable(GLenum cap) {
	switch (cap) {
	case GL_BLEND:
		g.blend = 1;
		break;
	case GL_SCISSOR_TEST:
		g.scissor_test = 1;
		break;
	case GL_DEPTH_TEST:
		g.depth_test = 1;
		break;
	case GL_CULL_FACE:
		g.cull_face_enabled = 1;
		break;
	default:
		break;
	}
}
void glDisable(GLenum cap) {
	switch (cap) {
	case GL_BLEND:
		g.blend = 0;
		break;
	case GL_SCISSOR_TEST:
		g.scissor_test = 0;
		break;
	case GL_DEPTH_TEST:
		g.depth_test = 0;
		break;
	case GL_CULL_FACE:
		g.cull_face_enabled = 0;
		break;
	default:
		break;
	}
}
GLboolean glIsEnabled(GLenum cap) {
	switch (cap) {
	case GL_BLEND:
		return g.blend ? GL_TRUE : GL_FALSE;
	case GL_SCISSOR_TEST:
		return g.scissor_test ? GL_TRUE : GL_FALSE;
	case GL_DEPTH_TEST:
		return g.depth_test ? GL_TRUE : GL_FALSE;
	case GL_CULL_FACE:
		return g.cull_face_enabled ? GL_TRUE : GL_FALSE;
	default:
		return GL_FALSE;
	}
}
void glBlendFunc(GLenum sfactor, GLenum dfactor) {
	g.blend_src = sfactor;
	g.blend_dst = dfactor;
	g.blend_src_a = sfactor;
	g.blend_dst_a = dfactor;
}
void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
	g.blend_src = srcRGB;
	g.blend_dst = dstRGB;
	g.blend_src_a = srcAlpha;
	g.blend_dst_a = dstAlpha;
}
void glBlendEquation(GLenum mode) {
	g.blend_equation = mode;
	g.blend_equation_a = mode;
}
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
	g.blend_equation = modeRGB;
	g.blend_equation_a = modeAlpha;
}
void glCullFace(GLenum mode) { g.cull_face = mode; }
void glFrontFace(GLenum mode) { g.front_face = mode; }
void glColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) {
	g.colormask = (r ? 1 : 0) | (gg ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}
void glDepthMask(GLboolean flag) { g.depth_mask = flag ? 1 : 0; }
void glDepthFunc(GLenum func) { g.depth_func = func; }
void glClearDepthf(GLfloat depth) { g.clear_depth = depth; }
void glClearColor(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
	g.clear[0] = r;
	g.clear[1] = gg;
	g.clear[2] = b;
	g.clear[3] = a;
}

/* Kodi renders GUI windows in dirty regions: SetScissors(region),
 * RenderPass(), glClear().  The GNM clear utility draws a full-screen quad,
 * so the screen scissor must be programmed from the current GL state before
 * the clear; otherwise the clear uses the scissor of the previous draw (or
 * the whole target), wiping the rest of the frame and leaving the dirty
 * regions as bands/flashes.  The same intersection is used for draws.
 *
 * clear=true follows GL semantics for glClear with SCISSOR_TEST disabled:
 * the whole render target is cleared (the viewport does not clip a clear).
 * clear=false keeps the viewport clip used by draws when the scissor is off. */
static void screen_scissor_rect(bool clear, GLint out[4]) {
	GnmRenderTarget* rt = current_render_target();
	const GLint tw = rt ? (GLint)rt->size.width : (GLint)g.display.screenw;
	const GLint th = rt ? (GLint)rt->size.height : (GLint)g.display.screenh;

	GLint left = 0, top = 0, right = tw, bottom = th;
	if (g.scissor_test) {
		const GLint w = g.sc[2] > 0 ? g.sc[2] : 0;
		const GLint h = g.sc[3] > 0 ? g.sc[3] : 0;
		left = g.sc[0];
		top = th - (g.sc[1] + h);
		right = left + w;
		bottom = top + h;
	} else if (!clear) {
		const GLint w = g.vp[2] > 0 ? g.vp[2] : 0;
		const GLint h = g.vp[3] > 0 ? g.vp[3] : 0;
		left = g.vp[0];
		top = th - (g.vp[1] + h);
		right = left + w;
		bottom = top + h;
	}

	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (left > tw)
		left = tw;
	if (top > th)
		top = th;
	if (right < left)
		right = left;
	if (right > tw)
		right = tw;
	if (bottom < top)
		bottom = top;
	if (bottom > th)
		bottom = th;

	out[0] = left;
	out[1] = top;
	out[2] = right;
	out[3] = bottom;
}

static void apply_screen_scissor(bool clear) {
	GLint scissor[4];
	screen_scissor_rect(clear, scissor);
	if (g.draw.scissor_test != g.scissor_test ||
	    memcmp(g.draw.sc, scissor, sizeof(scissor)) != 0) {
		gnmDrawCmdSetScreenScissor(&g.cmd, (uint32_t)scissor[0], (uint32_t)scissor[1],
					   (uint32_t)scissor[2], (uint32_t)scissor[3]);
		g.draw.scissor_test = g.scissor_test;
		memcpy(g.draw.sc, scissor, sizeof(scissor));
	}
}

void glClear(GLenum mask) {
	if (!g.ready)
		glShimInit(1920, 1080);
	if (!g.ready)
		return;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_CLEAR);
	if (mask & GL_COLOR_BUFFER_BIT)
		g_canary_clears++;
	if (CANARY_TAKE(CANARY_SLOT_CLEAR, 12))
		slog("canary: clear mask=0x%x fbo=%u clear=%.3f,%.3f,%.3f,%.3f",
		     mask, g.cur_fbo, g.clear[0], g.clear[1], g.clear[2], g.clear[3]);
#endif
	if (mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))
	{
		/* The GNM clear utility programs a full-target viewport, and its
		 * setupviewport() also rewrites the screen scissor to that full
		 * rectangle.  Pass the dirty-region rectangle into the clear so
		 * it is applied after the viewport, otherwise the scissor set by
		 * Kodi is lost and the clear wipes the whole frame. */
		GLint scissor[4];
		screen_scissor_rect(true, scissor);

		if (mask & GL_COLOR_BUFFER_BIT)
		{
			GnmRenderTarget* rt = current_render_target();
			if (rt)
				clearcolortarget_scissor(&g.cmd, rt, g.clear, scissor[0],
							 scissor[1], scissor[2],
							 scissor[3]);
		}
		/* Offscreen color targets have no depth attachment. */
		if ((mask & GL_DEPTH_BUFFER_BIT) && !framebuffer_by_id(g.cur_fbo))
			cleardepthtarget_scissor(&g.cmd, &g.depthtarget, g.clear_depth,
						 scissor[0], scissor[1], scissor[2],
						 scissor[3]);
		apply_screen_scissor(true);
		g.draw.valid = 0;
	}
}
GLenum glGetError(void) {
	GLenum e = g.error;
	g.error = GL_NO_ERROR;
	return e;
}
void glFinish(void) {
	/* Do not submit or flip here: Kodi calls glFinish/SyncGPU while a frame
	 * is being built, and flipping an incomplete frame causes flicker.  Wait
	 * only for the already submitted frame (the async present pipeline), so
	 * a texture released after SyncGPU cannot still be scanned by the GPU. */
	if (g.pipeline && g.flip_pending) {
		(void)displayctx_wait_flip_status(&g.display);
		g.flip_pending = 0;
	}
}
void glFlush(void) {}

static const GLubyte k_vendor[] = "Kodi PS4";
static const GLubyte k_renderer[] = "sceGnm glshim";
static const GLubyte k_version[] = "OpenGL ES 2.0 glshim";
static const GLubyte k_extensions[] = "";
const GLubyte* glGetString(GLenum name) {
	switch (name) {
	case GL_VENDOR:
		return k_vendor;
	case GL_RENDERER:
		return k_renderer;
	case GL_VERSION:
		return k_version;
	case GL_EXTENSIONS:
		return k_extensions;
	default:
		return NULL;
	}
}
const GLubyte* glGetStringi(GLenum name, GLuint index) {
	(void)name;
	(void)index;
	return NULL;
}
void glGetIntegerv(GLenum pname, GLint* params) {
	if (!params)
		return;
	switch (pname) {
	case GL_MAX_TEXTURE_SIZE:
		*params = 4096;
		break;
	case GL_MAX_TEXTURE_IMAGE_UNITS:
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
		*params = 8;
		break;
	case GL_MAX_VERTEX_ATTRIBS:
		*params = MAX_ATTRIBS;
		break;
	case GL_NUM_EXTENSIONS:
		*params = 0;
		break;
	case GL_VIEWPORT:
		memcpy(params, g.vp, sizeof(g.vp));
		break;
	case GL_SCISSOR_BOX:
		memcpy(params, g.sc, sizeof(g.sc));
		break;
	default:
		*params = 0;
		break;
	}
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

void glGenTextures(GLsizei n, GLuint* textures) {
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = 0;
		for (int j = 0; j < MAX_TEXTURES; j++) {
			if (!g.textures[j].used) {
				memset(&g.textures[j], 0, sizeof(ShimTexture));
				g.textures[j].used = 1;
				g.textures[j].id = (GLuint)j + 1;
				g.textures[j].minfilter = GL_LINEAR;
				g.textures[j].magfilter = GL_LINEAR;
				g.textures[j].wraps = GL_CLAMP_TO_EDGE;
				g.textures[j].wrapt = GL_CLAMP_TO_EDGE;
				id = g.textures[j].id;
				break;
			}
		}
		textures[i] = id;
	}
}
void glDeleteTextures(GLsizei n, const GLuint* textures) {
	for (GLsizei i = 0; i < n; i++) {
		ShimTexture* t = texture_by_id(textures[i]);
		if (!t)
			continue;
		/* An FBO referencing this texture becomes incomplete; the
		 * texture owns the color-surface memory and frees it below. */
		for (int j = 0; j < MAX_FRAMEBUFFERS; j++) {
			if (g.framebuffers[j].used && g.framebuffers[j].texid == t->id) {
				g.framebuffers[j].texid = 0;
				g.framebuffers[j].rt_valid = 0;
				g.framebuffers[j].width = g.framebuffers[j].height = 0;
				if (g.cur_fbo == g.framebuffers[j].id)
					g.cur_fbo = 0;
				g.draw.valid = 0;
			}
		}
		if (t->pixels)
			texture_canary_check(t, "delete");
		if (t->pixels)
			garbage_add(t->pixels);
		memset(t, 0, sizeof(*t));
	}
}
void glBindTexture(GLenum target, GLuint texture) {
	(void)target;
	GLuint unit = g.active_texture < 8 ? g.active_texture : 0;
	g.texunit[unit] = texture;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_BINDTEX);
	if (CANARY_TAKE(CANARY_SLOT_BINDTEX, 32)) {
		ShimTexture* t = texture_by_id(texture);
		slog("canary: bindtex unit=%u id=%u %dx%d bpp=%d", unit, texture,
		     t ? t->width : 0, t ? t->height : 0, t ? t->bpp : 0);
	}
#endif
}
void glActiveTexture(GLenum texture) {
	if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 8)
		g.active_texture = texture - GL_TEXTURE0;
}
GLboolean glIsTexture(GLuint texture) { return texture_by_id(texture) ? GL_TRUE : GL_FALSE; }
void glTexParameteri(GLenum target, GLenum pname, GLint param) {
	(void)target;
	ShimTexture* t = current_texture();
	if (!t)
		return;
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER:
		t->minfilter = param;
		break;
	case GL_TEXTURE_MAG_FILTER:
		t->magfilter = param;
		break;
	case GL_TEXTURE_WRAP_S:
		t->wraps = param;
		break;
	case GL_TEXTURE_WRAP_T:
		t->wrapt = param;
		break;
	case GL_TEXTURE_PS4_VIDEO_UNORM:
		t->video_unorm = param ? 1 : 0;
		break;
	default:
		break;
	}
}
void glPixelStorei(GLenum pname, GLint param) {
	if (pname == GL_UNPACK_ROW_LENGTH)
		g.unpack_row_length = param;
}
void glGenerateMipmap(GLenum target) { (void)target; }

/* Expand one source texture row to the stored texel format.
 * LUMINANCE and ALPHA are kept as one byte per texel (R8/A8) so video
 * planes do not pay a 4x memory-bandwidth expansion every frame. */
static void upload_row(uint8_t* dst, const uint8_t* src, GLsizei width, GLenum format,
		       GLenum type, GLint rowlength, int dst_bpp) {
	if (!dst || !src || width <= 0)
		return;
	if (dst_bpp == 2 && format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE) {
		/* RG8 chroma: the source is already interleaved L,A and the GNM
		 * format maps X->G, Y->A, so this is an exact two-byte copy. */
		memcpy(dst, src, (size_t)width * 2);
		return;
	}
	GLint n = rowlength > 0 ? rowlength : width;
	(void)n;
#if defined(__SSSE3__)
	if (dst_bpp == 4 && format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
		const __m128i swap = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11,
						   14, 13, 12, 15);
		GLsizei x = 0;
		for (; x + 4 <= width; x += 4) {
			__m128i v = _mm_loadu_si128((const __m128i*)(src + (size_t)x * 4));
			_mm_storeu_si128((__m128i*)(dst + (size_t)x * 4),
					 _mm_shuffle_epi8(v, swap));
		}
		for (; x < width; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
		return;
	}
	if (dst_bpp == 4 && format == GL_LUMINANCE && type == GL_UNSIGNED_BYTE) {
		const __m128i spread = _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
						     3, 3, 3, 3);
		const __m128i alpha = _mm_set1_epi32((int)0xff000000u);
		GLsizei x = 0;
		for (; x + 4 <= width; x += 4) {
			const __m128i v = _mm_cvtsi32_si128(*(const int*)(src + x));
			_mm_storeu_si128((__m128i*)(dst + (size_t)x * 4),
					 _mm_or_si128(_mm_shuffle_epi8(v, spread), alpha));
		}
		for (; x < width; x++) {
			uint8_t l = src[x];
			dst[x * 4 + 0] = l;
			dst[x * 4 + 1] = l;
			dst[x * 4 + 2] = l;
			dst[x * 4 + 3] = 255;
		}
		return;
	}
	if (dst_bpp == 4 && format == GL_ALPHA && type == GL_UNSIGNED_BYTE) {
		const __m128i spread = _mm_setr_epi8((char)0x80, (char)0x80, (char)0x80, 0,
						     (char)0x80, (char)0x80, (char)0x80, 1,
						     (char)0x80, (char)0x80, (char)0x80, 2,
						     (char)0x80, (char)0x80, (char)0x80, 3);
		const __m128i rgb = _mm_set1_epi32(0x00ffffff);
		GLsizei x = 0;
		for (; x + 4 <= width; x += 4) {
			const __m128i v = _mm_cvtsi32_si128(*(const int*)(src + x));
			_mm_storeu_si128((__m128i*)(dst + (size_t)x * 4),
					 _mm_or_si128(_mm_shuffle_epi8(v, spread), rgb));
		}
		for (; x < width; x++) {
			uint8_t a = src[x];
			dst[x * 4 + 0] = 255;
			dst[x * 4 + 1] = 255;
			dst[x * 4 + 2] = 255;
			dst[x * 4 + 3] = a;
		}
		return;
	}
	if (dst_bpp == 4 && format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE) {
		const __m128i spread = _mm_setr_epi8(0, 0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 5, 6, 6,
						     6, 7);
		GLsizei x = 0;
		for (; x + 4 <= width; x += 4) {
			const uint64_t v64 = *(const uint64_t*)(src + (size_t)x * 2);
			const __m128i v = _mm_cvtsi64_si128((long long)v64);
			_mm_storeu_si128((__m128i*)(dst + (size_t)x * 4),
					 _mm_shuffle_epi8(v, spread));
		}
		for (; x < width; x++) {
			uint8_t l = src[x * 2 + 0];
			uint8_t a = src[x * 2 + 1];
			dst[x * 4 + 0] = l;
			dst[x * 4 + 1] = l;
			dst[x * 4 + 2] = l;
			dst[x * 4 + 3] = a;
		}
		return;
	}
#endif
	if (dst_bpp == 1 && format == GL_LUMINANCE) {
		memcpy(dst, src, (size_t)width);
		return;
	}
	if (dst_bpp == 2 && format == GL_RED && type == GL_UNSIGNED_SHORT) {
		memcpy(dst, src, (size_t)width * 2);
		return;
	}
	if (type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB) {
		const uint16_t* s16 = (const uint16_t*)src;
		for (GLsizei x = 0; x < width; x++) {
			uint16_t v = s16[x];
			dst[x * 4 + 0] = (uint8_t)(((v >> 11) & 0x1f) * 255 / 31);
			dst[x * 4 + 1] = (uint8_t)(((v >> 5) & 0x3f) * 255 / 63);
			dst[x * 4 + 2] = (uint8_t)((v & 0x1f) * 255 / 31);
			dst[x * 4 + 3] = 255;
		}
		return;
	}
	if (type == GL_UNSIGNED_SHORT_5_5_5_1 && format == GL_RGBA) {
		const uint16_t* s16 = (const uint16_t*)src;
		for (GLsizei x = 0; x < width; x++) {
			uint16_t v = s16[x];
			dst[x * 4 + 0] = (uint8_t)(((v >> 11) & 0x1f) * 255 / 31);
			dst[x * 4 + 1] = (uint8_t)(((v >> 6) & 0x1f) * 255 / 31);
			dst[x * 4 + 2] = (uint8_t)(((v >> 1) & 0x1f) * 255 / 31);
			dst[x * 4 + 3] = (v & 1) ? 255 : 0;
		}
		return;
	}
	if (type == GL_UNSIGNED_SHORT_4_4_4_4 && format == GL_RGBA) {
		const uint16_t* s16 = (const uint16_t*)src;
		for (GLsizei x = 0; x < width; x++) {
			uint16_t v = s16[x];
			dst[x * 4 + 0] = (uint8_t)(((v >> 12) & 0xf) * 17);
			dst[x * 4 + 1] = (uint8_t)(((v >> 8) & 0xf) * 17);
			dst[x * 4 + 2] = (uint8_t)(((v >> 4) & 0xf) * 17);
			dst[x * 4 + 3] = (uint8_t)((v & 0xf) * 17);
		}
		return;
	}
	switch (format) {
	case GL_RGBA:
		memcpy(dst, src, (size_t)width * 4);
		break;
	case GL_BGRA_EXT:
		for (GLsizei x = 0; x < width; x++) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
		break;
	case GL_RGB:
		for (GLsizei x = 0; x < width; x++) {
			dst[x * 4 + 0] = src[x * 3 + 0];
			dst[x * 4 + 1] = src[x * 3 + 1];
			dst[x * 4 + 2] = src[x * 3 + 2];
			dst[x * 4 + 3] = 255;
		}
		break;
	case GL_ALPHA:
		for (GLsizei x = 0; x < width; x++) {
			uint8_t a = src[x];
			dst[x * 4 + 0] = 255;
			dst[x * 4 + 1] = 255;
			dst[x * 4 + 2] = 255;
			dst[x * 4 + 3] = a;
		}
		break;
	case GL_LUMINANCE:
		for (GLsizei x = 0; x < width; x++) {
			uint8_t l = src[x];
			dst[x * 4 + 0] = l;
			dst[x * 4 + 1] = l;
			dst[x * 4 + 2] = l;
			dst[x * 4 + 3] = 255;
		}
		break;
	case GL_LUMINANCE_ALPHA:
		for (GLsizei x = 0; x < width; x++) {
			uint8_t l = src[x * 2 + 0];
			uint8_t a = src[x * 2 + 1];
			dst[x * 4 + 0] = l;
			dst[x * 4 + 1] = l;
			dst[x * 4 + 2] = l;
			dst[x * 4 + 3] = a;
		}
		break;
	default:
		break;
	}
}

static int src_bpp(GLenum format, GLenum type) {
	switch (type) {
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT_4_4_4_4:
		return 2;
	default:
		break;
	}
	if (type == GL_UNSIGNED_SHORT)
		return 2;
	switch (format) {
	case GL_RGBA:
	case GL_BGRA_EXT:
		return 4;
	case GL_RGB:
		return 3;
	case GL_LUMINANCE_ALPHA:
		return 2;
	case GL_ALPHA:
	case GL_LUMINANCE:
		return 1;
	default:
		return 4;
	}
}

static int g_font_dump_count;

static void dump_font_atlas(const ShimTexture* t, const char* label, int x, int y, int w,
			    int h) {
	if (!t || !t->pixels || g_font_dump_count >= 10)
		return;
	char path[64];
	snprintf(path, sizeof(path), "/data/kodi-ps4/font-atlas-%d.raw", g_font_dump_count);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		(void)write(fd, t->pixels, t->bytes);
		close(fd);
		slog("font: atlas dump %d %s id=%u texw=%d texh=%d x=%d y=%d w=%d h=%d bytes=%zu",
		     g_font_dump_count, label, t->id, t->width, t->height, x, y, w, h, t->bytes);
		g_font_dump_count++;
	}
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
		  GLsizei height, GLint border, GLenum format, GLenum type,
		  const void* pixels) {
	if (!g.ready)
		glShimInit(1920, 1080);
	const uint64_t tex_t0 = sceKernelGetProcessTime();
	(void)target;
	(void)internalformat;
	(void)border;
	(void)type;
	ShimTexture* t = current_texture();
	if (!t || level != 0 || width <= 0 || height <= 0) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (width > SHIM_MAX_TEXTURE_SIZE || height > SHIM_MAX_TEXTURE_SIZE) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	/* A positive row length shorter than the logical width makes upload_row
	 * read beyond the caller's row.  Reject it instead of guessing. */
	if (g.unpack_row_length > 0 && g.unpack_row_length < width) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	const GLsizei pitch = (width + 63) & ~63;
	const GLsizei rows = (height + 7) & ~7;
	const bool is_r16 = (format == GL_RED && type == GL_UNSIGNED_SHORT);
	// Only use the one-byte R8 RRR1 path for the large video planes.  Small
	// GL_LUMINANCE GUI textures (menu icons) expect the full GLES
	// luminance/alpha semantics and are safer as RGBA8.  GL_ALPHA stays
	// RGBA8: the GNM A8 channel select was not honoured by the sampler on
	// this path and V came out as 1.0 (magenta video).
	const bool fast_luma =
	    (format == GL_LUMINANCE && (width >= 512 || height >= 512));
	/* NV12 chroma is a large LUMINANCE_ALPHA plane.  Keep it at 2 B/texel
	 * (RG8) unless the A/B toggle asks for the old 4 B RGBA8 path. */
	const bool fast_la = (format == GL_LUMINANCE_ALPHA &&
			      (width >= 512 || height >= 512) && t->video_unorm &&
			      !g.la_rgba8);
	const int dst_bpp = is_r16 ? 2 : (fast_luma ? 1 : (fast_la ? 2 : 4));
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_TEX);
	if (CANARY_TAKE(CANARY_SLOT_TEX, 16)) {
		unsigned sum = 0;
		size_t nz = 0, n = 0;
		if (pixels && width > 0 && height > 0) {
			const int sbpp = src_bpp(format, type);
			n = (size_t)width * (size_t)height * (size_t)(sbpp > 0 ? sbpp : 1);
			if (n > 4096)
				n = 4096;
			const uint8_t* p = (const uint8_t*)pixels;
			for (size_t i = 0; i < n; i++) {
				sum += p[i];
				if (p[i])
					nz++;
			}
		}
		slog("canary: tex2d fmt=0x%x type=0x%x %dx%d bpp=%d pixels=%p sum=%u nz=%zu/%zu",
		     format, type, width, height, dst_bpp, pixels, sum, nz, n);
	}
#endif
	const size_t bytes = (size_t)pitch * (size_t)rows * (size_t)dst_bpp;
	const size_t alloc_bytes = bytes + sizeof(uint64_t) + (bytes & 1u);
	uint8_t* mem = memalloc_alloc(&g.mem, alloc_bytes, GNM_ALIGNMENT_SHADER_BYTES);
	if (!mem) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	memset(mem, 0, alloc_bytes);
	if (pixels) {
		const int bpp = src_bpp(format, type);
		const GLint rowlen = g.unpack_row_length;
		for (GLsizei y = 0; y < height; y++) {
			const uint8_t* src = (const uint8_t*)pixels +
					     (size_t)y * (size_t)(rowlen > 0 ? rowlen : width) * bpp;
			upload_row(mem + (size_t)y * pitch * (size_t)dst_bpp, src, width, format, type,
				   rowlen, dst_bpp);
		}
	}

	const GnmDataFormat store_format =
	    is_r16 ? GNM_FMT_R16_UNORM
		   : (dst_bpp == 1
			  ? GNM_FMT_R8_RRR1_UNORM
			  : (fast_la
				 ? GNM_FMT_R8G8_UV_GA_UNORM
				 : ((format == GL_LUMINANCE_ALPHA &&
				     (width >= 512 || height >= 512))
					? GNM_FMT_R8G8B8A8_UNORM
					: (t->video_unorm ? GNM_FMT_R8G8B8A8_UNORM
							  : GNM_FMT_R8G8B8A8_SRGB))));
	const GnmTextureCreateInfo ci = {
	    .texturetype = GNM_TEXTURE_2D,
	    .width = pitch,
	    .height = rows,
	    .depth = 1,
	    .pitch = pitch,
	    .nummiplevels = 1,
	    .numslices = 1,
	    .format = store_format,
	    .tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL,
	    .mingpumode = GNM_GPU_BASE,
	    .numfragments = 1,
	};
	GnmTexture tex = {0};
	if (gnmCreateTexture(&tex, &ci) != GNM_ERROR_OK) {
		memalloc_free(&g.mem, mem);
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	gnmTexSetBaseAddress(&tex, mem);
	/* Font atlases are written by the CPU after the texture was created.
	 * Keep them out of the GPU L1/L2 caches; otherwise the sampler can keep
	 * reading stale texels even though the CPU atlas (and the .raw dump) is
	 * correct.  Static GUI textures stay read-only/cached. */
	if (format == GL_ALPHA && width >= 512)
		/* The atlas is updated in place with glTexSubImage2D after the first
		 * draw.  Coherent L2 was still returning stale glyph rows on the
		 * console, so bypass L2 entirely for this dynamic texture. */
		gnmTexSetMemoryType(&tex, GNM_MEMORY_UNCACHED, true);
	else
		gnmTexSetMemoryType(&tex, GNM_MEMORY_READONLY, true);
	/* Pad the allocation, but expose only the logical area to normalized UVs. */
	gnmTexSetWidth(&tex, (uint32_t)width);
	gnmTexSetHeight(&tex, (uint32_t)height);
	if (t->pixels) {
		texture_canary_check(t, "replace");
		garbage_add(t->pixels);
	}
	t->pixels = mem;
	t->bytes = bytes;
	texture_canary_write(t);
	t->bpp = dst_bpp;
	t->tex = tex;
	t->width = width;
	t->height = height;
	t->dynamic = (format == GL_ALPHA && width >= 512);
	g.textures_dirty = 1;
	g.cache_flush_needed = 1;
	if (fast_la) {
		static int la_rg8_logged;
		if (la_rg8_logged < 8) {
			slog("tex: la rg8 %dx%d pitch=%d rows=%d bytes=%zu", width, height,
			     pitch, rows, bytes);
			la_rg8_logged++;
		}
	}
	if (t->dynamic) {
		static int fontimg_logged;
		if (g_font_dump_enabled && fontimg_logged < 40) {
			slog("fontimg: id=%u w=%d h=%d pitch=%u tile=0x%x base=%p", t->id,
			     t->width, t->height, gnmTexGetPitch(&t->tex),
			     t->tex.tilingindex, gnmTexGetBaseAddress(&t->tex));
			fontimg_logged++;
		}
	}
	if (g_font_dump_enabled && format == GL_ALPHA && pixels && width >= 512)
		dump_font_atlas(t, "image", 0, 0, width, height);
	g_perf_tex_us += sceKernelGetProcessTime() - tex_t0;
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
		     GLsizei width, GLsizei height, GLenum format, GLenum type,
		     const void* pixels) {
	const uint64_t tex_t0 = sceKernelGetProcessTime();
	(void)target;
	(void)level;
	(void)type;
	ShimTexture* t = current_texture();
	if (!t || !t->pixels || !pixels) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (width <= 0 || height <= 0 || xoffset < 0 || yoffset < 0 ||
	    xoffset > t->width || yoffset > t->height ||
	    width > t->width - xoffset || height > t->height - yoffset) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	if (g.unpack_row_length > 0 && g.unpack_row_length < width) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	/* The destination row is allocated with t->bpp bytes per texel.  The
	 * generic upload_row paths write 4 bytes for RGBA/RGB/ALPHA, so a
	 * format different from the creation format could overrun a 1/2-byte
	 * row.  Only accept formats whose stored representation matches. */
	const int source_bpp = src_bpp(format, type);
	if (source_bpp <= 0 ||
	    (t->bpp != 1 && t->bpp != 2 && t->bpp != 4) ||
	    (t->bpp == 1 && !(format == GL_LUMINANCE && type == GL_UNSIGNED_BYTE)) ||
	    (t->bpp == 2 &&
	     !((format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE) ||
	       (format == GL_RED && type == GL_UNSIGNED_SHORT)))) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}

	/* Font atlases grow in place.  The texture is marked coherent/uncached
	 * and the frame starts with a cache flush+invalidate, so there is no
	 * need to copy the whole atlas (up to 1 MB) on every glyph upload. */
	const GLsizei pitch = (t->width + 63) & ~63;
	const int bpp = src_bpp(format, type);
	const GLint rowlen = g.unpack_row_length;
	const int dst_bpp = t->bpp > 0 ? t->bpp : 4;
	for (GLsizei y = 0; y < height; y++) {
		const uint8_t* src = (const uint8_t*)pixels +
				     (size_t)y * (size_t)(rowlen > 0 ? rowlen : width) * bpp;
		upload_row(t->pixels + (size_t)(yoffset + y) * pitch * (size_t)dst_bpp +
			       (size_t)xoffset * (size_t)dst_bpp,
			   src, width, format, type, rowlen, dst_bpp);
	}
	texture_canary_check(t, "subimage");
	g.textures_dirty = 1;
	g.cache_flush_needed = 1;
	if (format == GL_ALPHA && t->width >= 512) {
		/* Make the new glyph rows visible before the draw that references
		 * them; waiting until present was too late on some runs. */
		sceGnmFlushGarlic();
		static int fontsub_logged;
		if (g_font_dump_enabled && fontsub_logged < 200) {
			slog("fontsub: id=%u texw=%d texh=%d base=%p tile=0x%x mtype=%u/%u/%u x=%d y=%d w=%d h=%d",
			     t->id, t->width, t->height, gnmTexGetBaseAddress(&t->tex),
			     t->tex.tilingindex, t->tex.mtype_l2, t->tex.mtype0,
			     t->tex.mtype2, xoffset, yoffset, width, height);
			fontsub_logged++;
		}
		if (g_font_dump_enabled)
			dump_font_atlas(t, "sub", xoffset, yoffset, width, height);
	}
	g_perf_tex_us += sceKernelGetProcessTime() - tex_t0;
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
			    GLsizei width, GLsizei height, GLint border,
			    GLsizei imageSize, const void* data) {
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)border;
	(void)imageSize;
	(void)data;
	shim_set_error(GL_INVALID_ENUM);
}

/* ------------------------------------------------------------------ */
/* buffers and attributes                                              */
/* ------------------------------------------------------------------ */

void glGenBuffers(GLsizei n, GLuint* buffers) {
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = 0;
		for (int j = 0; j < MAX_BUFFERS; j++) {
			if (!g.buffers[j].used) {
				memset(&g.buffers[j], 0, sizeof(ShimBuffer));
				g.buffers[j].used = 1;
				g.buffers[j].id = (GLuint)j + 1;
				id = g.buffers[j].id;
				break;
			}
		}
		buffers[i] = id;
	}
}
void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
	for (GLsizei i = 0; i < n; i++) {
		ShimBuffer* b = buffer_by_id(buffers[i]);
		if (!b)
			continue;
		if (b->data)
			garbage_add(b->data);
		memset(b, 0, sizeof(*b));
	}
}
void glBindBuffer(GLenum target, GLuint buffer) {
	if (target == GL_ARRAY_BUFFER)
		g.array_buffer = buffer;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		g.element_buffer = buffer;
}
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
	if (!g.ready)
		glShimInit(1920, 1080);
	(void)usage;
	if (size < 0) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	GLuint id = target == GL_ELEMENT_ARRAY_BUFFER ? g.element_buffer : g.array_buffer;
	ShimBuffer* b = buffer_by_id(id);
	if (!b)
		return;
	/* Kodi's buffer arena respecifies the same 16 KB buffers every frame.
	 * Reuse the allocation when the size is unchanged; the full buffer is
	 * rewritten from the CPU copy, so earlier draws keep their slots. */
	if (b->data && b->size == (size_t)size) {
		if (data)
			memcpy(b->data, data, (size_t)size);
		else
			memset(b->data, 0, (size_t)size);
		return;
	}
	void* mem = memalloc_alloc(&g.mem, (size_t)(size ? size : 4),
				   GNM_ALIGNMENT_BUFFER_BYTES);
	if (!mem) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (data)
		memcpy(mem, data, (size_t)size);
	else
		memset(mem, 0, (size_t)size);
	if (b->data)
		garbage_add(b->data);
	b->data = mem;
	b->size = (size_t)size;
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
	GLuint id = target == GL_ELEMENT_ARRAY_BUFFER ? g.element_buffer : g.array_buffer;
	ShimBuffer* b = buffer_by_id(id);
	if (!b || !data || offset < 0 || size < 0 ||
	    (size_t)offset > b->size || (size_t)size > b->size - (size_t)offset) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	/* The GPU is done with the previous frame (EOP wait) and Kodi uploads
	 * new vertex ranges instead of overwriting live ranges, so an in-place
	 * update is safe and avoids copying the whole buffer per upload. */
	memcpy((uint8_t*)b->data + offset, data, (size_t)size);
}

void glEnableVertexAttribArray(GLuint index) {
	if (index < MAX_ATTRIBS)
		g.attribs[index].enabled = 1;
}
void glDisableVertexAttribArray(GLuint index) {
	if (index < MAX_ATTRIBS)
		g.attribs[index].enabled = 0;
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
			   GLsizei stride, const void* pointer) {
	if (index >= MAX_ATTRIBS) {
		shim_set_error(GL_INVALID_VALUE);
		return;
	}
	ShimAttrib* a = &g.attribs[index];
	a->size = size;
	a->type = type;
	a->normalized = normalized;
	a->stride = stride;
	a->offset = (size_t)(uintptr_t)pointer;
	a->buffer = g.array_buffer;
	a->client = (g.array_buffer == 0);
}

static const GnmDataFormat* attrib_format(const ShimAttrib* a) {
	static const GnmDataFormat fmt_r16g16_uscaled = GNM_DATA_FORMAT_INIT(
	    GNM_IMG_DATA_FORMAT_16_16, GNM_IMG_NUM_FORMAT_USCALED, GNM_CHAN_X,
	    GNM_CHAN_Y, GNM_CHAN_Z, GNM_CHAN_W);
	static const GnmDataFormat fmt_r8g8b8a8_uscaled = GNM_DATA_FORMAT_INIT(
	    GNM_IMG_DATA_FORMAT_8_8_8_8, GNM_IMG_NUM_FORMAT_USCALED, GNM_CHAN_X,
	    GNM_CHAN_Y, GNM_CHAN_Z, GNM_CHAN_W);
	static const GnmDataFormat fmt_r16g16_unorm = GNM_DATA_FORMAT_INIT(
	    GNM_IMG_DATA_FORMAT_16_16, GNM_IMG_NUM_FORMAT_UNORM, GNM_CHAN_X,
	    GNM_CHAN_Y, GNM_CHAN_Z, GNM_CHAN_W);
	switch (a->type) {
	case GL_FLOAT:
		switch (a->size) {
		case 1:
			return &GNM_FMT_R32_FLOAT;
		case 2:
			return &GNM_FMT_R32G32_FLOAT;
		case 3:
			return &GNM_FMT_R32G32B32_FLOAT;
		default:
			return &GNM_FMT_R32G32B32A32_FLOAT;
		}
	case GL_UNSIGNED_BYTE:
		if (a->size == 4)
			return a->normalized ? &GNM_FMT_R8G8B8A8_UNORM : &fmt_r8g8b8a8_uscaled;
		break;
	case GL_UNSIGNED_SHORT:
		if (a->size == 2)
			return a->normalized ? &fmt_r16g16_unorm : &fmt_r16g16_uscaled;
		break;
	default:
		break;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* shaders / programs                                                  */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static const char* pfs_name(const uint8_t* block, size_t fieldoff,
			    const uint8_t* psb_end) {
	const uint8_t* field = block + fieldoff;
	if (field > psb_end || (size_t)(psb_end - field) < 4)
		return "";
	const uint32_t off = rd32(field);
	if ((size_t)off > (size_t)(psb_end - field) - 4)
		return "";
	const char* name = (const char*)(field + off);
	const size_t maxlen = (size_t)(psb_end - (const uint8_t*)name);
	if (!memchr(name, '\0', maxlen))
		return "";
	return name;
}

static int blob_to_psb(const uint8_t* d, size_t n, const uint8_t** psb, size_t* psblen) {
	if (n >= 20 && rd32(d) == 0xE891BC71u) {
		uint32_t srclen = rd32(d + 12);
		size_t off = 16 + srclen;
		if (off + 4 > n)
			return 0;
		uint32_t plen = rd32(d + off);
		if (off + 4 + plen > n)
			return 0;
		*psb = d + off + 4;
		*psblen = plen;
		return 1;
	}
	if (n > 40 && d[36] == 'S' && d[37] == 'h' && d[38] == 'd' && d[39] == 'r') {
		*psb = d;
		*psblen = n;
		return 1;
	}
	return 0;
}

GLuint glCreateShader(GLenum type) {
	static GLuint next = 1;
	for (int i = 0; i < MAX_SHADERS; i++) {
		if (!g_shaders[i].used) {
			memset(&g_shaders[i], 0, sizeof(ShimShader));
			g_shaders[i].used = 1;
			g_shaders[i].id = next++;
			g_shaders[i].type = type;
			return g_shaders[i].id;
		}
	}
	return 0;
}
void glShaderBinary(GLsizei count, const GLuint* shaders, GLenum binaryformat,
		    const void* binary, GLsizei length) {
	(void)binaryformat;
	for (GLsizei i = 0; i < count; i++) {
		ShimShader* sh = shader_by_id(shaders[i]);
		if (!sh)
			continue;
		uint8_t* mem = memalloc_alloc(&g.mem, (size_t)length,
					      GNM_ALIGNMENT_BUFFER_BYTES);
		if (!mem) {
			shim_set_error(GL_INVALID_OPERATION);
			continue;
		}
		memcpy(mem, binary, (size_t)length);
		if (sh->blob)
			memalloc_free(&g.mem, sh->blob);
		sh->blob = mem;
		sh->bloblen = (size_t)length;
	}
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
		    const GLint* length) {
	(void)count;
	(void)string;
	(void)length;
	ShimShader* sh = shader_by_id(shader);
	if (sh)
		sh->hassource = 1;
}
void glCompileShader(GLuint shader) { (void)shader; }
void glDeleteShader(GLuint shader) {
	ShimShader* sh = shader_by_id(shader);
	if (!sh)
		return;
	if (sh->blob)
		memalloc_free(&g.mem, sh->blob);
	memset(sh, 0, sizeof(*sh));
}

GLuint glCreateProgram(void) {
	static GLuint next = 1;
	for (int i = 0; i < MAX_PROGRAMS; i++) {
		if (!g_programs[i].used) {
			memset(&g_programs[i], 0, sizeof(ShimProgram));
			g_programs[i].used = 1;
			g_programs[i].id = next++;
			return g_programs[i].id;
		}
	}
	/* A zero handle is fatal for Kodi's shader setup; make the cause
	 * explicit instead of leaving only "Error creating shader program
	 * handle" in the log. */
	slog("glCreateProgram: program table full (%d slots)", MAX_PROGRAMS);
	return 0;
}
void glAttachShader(GLuint program, GLuint shader) {
	ShimProgram* p = program_by_id(program);
	ShimShader* sh = shader_by_id(shader);
	if (!p || !sh)
		return;
	if (sh->type == GL_VERTEX_SHADER)
		p->vs_id = shader;
	else if (sh->type == GL_FRAGMENT_SHADER)
		p->ps_id = shader;
}
void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name) {
	(void)program;
	(void)index;
	(void)name;
}

static void program_add_attr(ShimProgram* p, const char* name, GLint loc) {
	if (p->nattrs >= MAX_ATTRS)
		return;
	snprintf(p->attrs[p->nattrs].name, sizeof(p->attrs[p->nattrs].name), "%s", name);
	p->attrs[p->nattrs].loc = loc;
	p->nattrs++;
}
static void program_add_uni(ShimProgram* p, const char* name, uint32_t off, uint8_t etype,
			    uint8_t stage) {
	if (p->nunis >= MAX_UNIS)
		return;
	snprintf(p->unis[p->nunis].name, sizeof(p->unis[p->nunis].name), "%s", name);
	p->unis[p->nunis].off = off;
	p->unis[p->nunis].etype = etype;
	p->unis[p->nunis].stage = stage;
	p->nunis++;
	if (strcmp(name, "m_yuvmat") == 0) {
		p->has_yuvmat = 1;
		p->yuvmat_off = off;
		p->yuvmat_stage = stage;
	}
}

static int parse_reflection(ShimProgram* p, const uint8_t* psb, size_t len,
			    int is_vertex) {
	if (len < 36)
		return 0;
	const uint32_t codesize = rd32(psb + 16);
	if (codesize > len - 36)
		return 0;
	const uint8_t* pi = psb + 36 + codesize;
	const size_t remaining = len - 36 - codesize;
	if (remaining < 28)
		return 0;

	const uint32_t nb = rd32(pi), nc = rd32(pi + 4), nv = rd32(pi + 8);
	const uint32_t nia = pi[16];
	const uint8_t* base = pi + 28;

	size_t avail = remaining - 28;
	if (nb > avail / 24)
		return 0;
	avail -= (size_t)nb * 24;
	if (nc > avail / 32)
		return 0;
	avail -= (size_t)nc * 32;
	if (nv > avail / 36)
		return 0;
	avail -= (size_t)nv * 36;
	if (is_vertex && nia > avail / 16)
		return 0;

	size_t maxend = 64;
	for (uint32_t i = 0; i < nb; i++) {
		const uint8_t* b = base + 24 * i;
		const char* name = pfs_name(b, 20, psb + len);
		if (b[9] == 5 && p->nsamplers < MAX_SAMPLERS) {
			snprintf(p->samplers[p->nsamplers].name,
				 sizeof(p->samplers[p->nsamplers].name), "%s", name);
			p->samplers[p->nsamplers].off = 0x10000u + i;
			p->sampler_unit[p->nsamplers] = p->nsamplers;
			p->nsamplers++;
		}
	}
	base += 24 * nb;
	for (uint32_t i = 0; i < nc; i++) {
		const uint8_t* c = base + 32 * i;
		const uint32_t off = rd32(c);
		if (off > CBUF_SIZE - 64)
			return 0;
		program_add_uni(p, pfs_name(c, 28, psb + len), off, c[9], is_vertex ? 0 : 1);
		if (off + 64 > maxend)
			maxend = off + 64;
	}
	base += 32 * nc;
	for (uint32_t i = 0; i < nv; i++) {
		const uint8_t* v = base + 36 * i;
		const uint32_t off = rd32(v + 4);
		if (off > CBUF_SIZE - 64)
			return 0;
		program_add_uni(p, pfs_name(v, 28, psb + len), off, v[0], is_vertex ? 0 : 1);
		if (off + 64 > maxend)
			maxend = off + 64;
	}
	base += 36 * nv;
	if (is_vertex) {
		for (uint32_t i = 0; i < nia; i++) {
			const uint8_t* a = base + 16 * i;
			program_add_attr(p, pfs_name(a, 8, psb + len), a[3]);
		}
	}
	maxend = (maxend + 15) & ~(size_t)15;
	if (is_vertex)
		p->vs_cbuf_size = maxend;
	else
		p->ps_cbuf_size = maxend;
	return 1;
}

/* Mirrors loadvshader/loadpshader: copy the stage header, copy the code to
 * GPU-visible memory and rebase the shader program address. */
static int load_stage(const uint8_t* psb, size_t len, int is_vertex, void** stage_out,
		      void** code_out) {
	const size_t shoff = sizeof(PsslBinaryHeader) + sizeof(GnmShaderFileHeader);
	const size_t stage_fixed = is_vertex ? sizeof(GnmVsShader) : sizeof(GnmPsShader);
	if (len < shoff + stage_fixed)
		return 0;
	const uint8_t* shcommon = psb + shoff;
	const GnmShaderCommonData* common = (const GnmShaderCommonData*)shcommon;
	if (common->numinputusageslots > 64)
		return 0;
	if (is_vertex) {
		const GnmVsShader* vs = (const GnmVsShader*)shcommon;
		if (vs->numinputsemantics > 64 || vs->numexportsemantics > 64)
			return 0;
	} else {
		const GnmPsShader* ps = (const GnmPsShader*)shcommon;
		if (ps->numinputsemantics > 64)
			return 0;
	}
	const uint32_t* sbaddr = (const uint32_t*)(shcommon + sizeof(GnmShaderCommonData));
	if ((sbaddr[0] & 3) || sbaddr[1] != 0xffffffffu || sbaddr[0] > len - shoff)
		return 0;
	const uint32_t* src = (const uint32_t*)(shcommon + sbaddr[0]);
	const size_t srcoff = (size_t)((const uint8_t*)src - psb);
	const uint32_t codesize = gnmShaderCommonCodeSize(common);
	const uint32_t stagesize =
	    is_vertex ? gnmVsShaderCalcSize((const GnmVsShader*)shcommon)
		      : gnmPsShaderCalcSize((const GnmPsShader*)shcommon);
	if (codesize > len - srcoff || stagesize > len - shoff)
		return 0;
	void* stage = memalloc_alloc(&g.mem, stagesize, 4);
	void* code = memalloc_alloc(&g.mem, codesize, GNM_ALIGNMENT_SHADER_BYTES);
	if (!stage || !code)
		return 0;
	memcpy(stage, shcommon, stagesize);
	memcpy(code, src, codesize);
	if (is_vertex) {
		gnmVsRegsSetAddress(&((GnmVsShader*)stage)->registers, code);
	} else {
		gnmPsRegsSetAddress(&((GnmPsShader*)stage)->registers, code);
		((GnmPsShader*)stage)->registers.spibaryccntl = 0;
	}
	*stage_out = stage;
	*code_out = code;
	return 1;
}

static int program_build(ShimProgram* p) {
	ShimShader* vssh = shader_by_id(p->vs_id);
	ShimShader* pssh = shader_by_id(p->ps_id);
	if (!vssh || !pssh || !vssh->blob || !pssh->blob) {
		slog("build: missing binary vs=%p ps=%p", vssh ? vssh->blob : NULL,
		     pssh ? pssh->blob : NULL);
		return 0;
	}
	const uint8_t *vpsb, *ppsb;
	size_t vlen, plen;
	if (!blob_to_psb(vssh->blob, vssh->bloblen, &vpsb, &vlen) ||
	    !blob_to_psb(pssh->blob, pssh->bloblen, &ppsb, &plen)) {
		slog("build: blob parse failed vs=%zu ps=%zu", vssh->bloblen, pssh->bloblen);
		return 0;
	}
	if (!load_stage(vpsb, vlen, 1, &p->vsmem, &p->vscode) ||
	    !load_stage(ppsb, plen, 0, &p->psmem, &p->pscode)) {
		slog("build: load_stage failed");
		return 0;
	}
	p->vsh = (GnmVsShader*)p->vsmem;
	p->psh = (GnmPsShader*)p->psmem;

	const GnmInputUsageSlot* vs_slots = gnmVsShaderInputUsageSlotTable(p->vsh);
	for (int i = 0; i < p->vsh->common.numinputusageslots; i++) {
		if (vs_slots[i].usagetype == GNM_SHINPUTUSAGE_SUBPTR_FETCHSHADER)
			p->vs_fetch_slot = vs_slots[i].startregister;
		else if (vs_slots[i].usagetype == GNM_SHINPUTUSAGE_PTR_VERTEXBUFFERTABLE)
			p->vs_vb_slot = vs_slots[i].startregister;
		else if (vs_slots[i].usagetype == GNM_SHINPUTUSAGE_IMM_CONSTBUFFER) {
			p->vs_const_slot = vs_slots[i].startregister;
			p->has_vs_const = 1;
		}
	}
	const GnmInputUsageSlot* ps_slots = gnmPsShaderInputUsageSlotTable(p->psh);
	for (int i = 0; i < p->psh->common.numinputusageslots; i++) {
		if (ps_slots[i].usagetype == GNM_SHINPUTUSAGE_IMM_CONSTBUFFER) {
			p->ps_const_slot = ps_slots[i].startregister;
			p->has_ps_const = 1;
		} else if (ps_slots[i].usagetype == GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE ||
			   ps_slots[i].usagetype == GNM_SHINPUTUSAGE_PTR_RESOURCETABLE) {
			p->ps_res_slot = ps_slots[i].startregister;
			p->has_ps_res = 1;
		}
	}

	p->cbufsize = CBUF_SIZE;
	p->cbuf_vs = memalloc_alloc(&g.mem, p->cbufsize, GNM_ALIGNMENT_BUFFER_BYTES);
	p->cbuf_ps = memalloc_alloc(&g.mem, p->cbufsize, GNM_ALIGNMENT_BUFFER_BYTES);
	if (!p->cbuf_vs || !p->cbuf_ps)
		return 0;
	memset(p->cbuf_vs, 0, p->cbufsize);
	memset(p->cbuf_ps, 0, p->cbufsize);

	const GnmFetchShaderCreateInfo fetchci = {
	    .regs = &p->vsh->registers,
	    .vtxinputs = gnmVsShaderInputSemanticTable(p->vsh),
	    .numvtxinputs = p->vsh->numinputsemantics,
	    .inputusages = vs_slots,
	    .numinputusages = p->vsh->common.numinputusageslots,
	};
	uint32_t fetchsize = 0;
	GnmError ferr = gnmFetchShaderCalcSize(&fetchsize, &fetchci);
	if (ferr != GNM_ERROR_OK) {
		slog("build: fetch calc failed %s", gnmStrError(ferr));
		return 0;
	}
	p->fetchmem = memalloc_alloc(&g.mem, fetchsize, GNM_ALIGNMENT_FETCHSHADER_BYTES);
	if (!p->fetchmem)
		return 0;
	GnmFetchShaderResults fetchres = {0};
	ferr = gnmCreateFetchShader(p->fetchmem, fetchsize, &fetchci, &fetchres);
	if (ferr != GNM_ERROR_OK) {
		slog("build: fetch create failed %s", gnmStrError(ferr));
		return 0;
	}
	gnmVsRegsSetFetchShaderModifier(&p->vsh->registers, &fetchres);

	if (!parse_reflection(p, vpsb, vlen, 1) ||
	    !parse_reflection(p, ppsb, plen, 0)) {
		slog("build: reflection parse failed vs=%zu ps=%zu", vlen, plen);
		return 0;
	}
	if (p->nattrs == 4 && p->nsamplers == 1) {
		slog("fontprog: id=%u attrs=%d unis=%d samplers=%d", p->id, p->nattrs,
		     p->nunis, p->nsamplers);
		for (int i = 0; i < p->nattrs; i++)
			slog("fontprog: attr[%d] name=%s loc=%d", i, p->attrs[i].name,
			     p->attrs[i].loc);
		for (int i = 0; i < p->nunis; i++)
			slog("fontprog: uni[%d] name=%s off=0x%x stage=%d", i,
			     p->unis[i].name, p->unis[i].off, p->unis[i].stage);
		for (int i = 0; i < p->nsamplers; i++)
			slog("fontprog: samp[%d] name=%s off=0x%x", i,
			     p->samplers[i].name, p->samplers[i].off);
	}
	if (p->nsamplers >= 3) {
		slog("samplerprog: id=%u attrs=%d unis=%d samplers=%d", p->id, p->nattrs,
		     p->nunis, p->nsamplers);
		for (int i = 0; i < p->nsamplers; i++)
			slog("samplerprog: samp[%d] name=%s off=0x%x", i,
			     p->samplers[i].name, p->samplers[i].off);
	}
	slog("build: ok attrs=%d unis=%d samplers=%d cbuf vs=%zu ps=%zu slots vc=%d pc=%d pr=%d vblob=%zu pblob=%zu",
	     p->nattrs, p->nunis, p->nsamplers, p->vs_cbuf_size, p->ps_cbuf_size,
	     p->has_vs_const, p->has_ps_const, p->has_ps_res, vssh->bloblen, pssh->bloblen);
	return 1;
}

void glLinkProgram(GLuint program) {
	ShimProgram* p = program_by_id(program);
	if (!p)
		return;
	p->linked = program_build(p);
	if (!p->linked)
		shim_set_error(GL_INVALID_OPERATION);
}
void glValidateProgram(GLuint program) { (void)program; }
void glUseProgram(GLuint program) {
	g.program = program;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_PROG);
	if (CANARY_TAKE(CANARY_SLOT_PROG, 12))
		slog("canary: use program=%u", program);
#endif
}
void glDeleteProgram(GLuint program) {
	ShimProgram* p = program_by_id(program);
	if (!p)
		return;
	if (p->vsmem)
		memalloc_free(&g.mem, p->vsmem);
	if (p->psmem)
		memalloc_free(&g.mem, p->psmem);
	if (p->vscode)
		memalloc_free(&g.mem, p->vscode);
	if (p->pscode)
		memalloc_free(&g.mem, p->pscode);
	if (p->fetchmem)
		memalloc_free(&g.mem, p->fetchmem);
	if (p->cbuf_vs)
		memalloc_free(&g.mem, p->cbuf_vs);
	if (p->cbuf_ps)
		memalloc_free(&g.mem, p->cbuf_ps);
	memset(p, 0, sizeof(*p));
}

GLint glGetAttribLocation(GLuint program, const GLchar* name) {
	ShimProgram* p = program_by_id(program);
	if (!p || !name)
		return -1;
	for (int i = 0; i < p->nattrs; i++)
		if (strcmp(p->attrs[i].name, name) == 0)
			return p->attrs[i].loc;
	return -1;
}
GLint glGetUniformLocation(GLuint program, const GLchar* name) {
	ShimProgram* p = program_by_id(program);
	if (!p || !name)
		return -1;
	for (int i = 0; i < p->nunis; i++) {
		GLint loc = (GLint)p->unis[i].off + (p->unis[i].stage ? 0x20000 : 0);
		if (strcmp(p->unis[i].name, name) == 0) {
			if (strcmp(name, "m_cordStep") == 0) {
				static int logged;
				if (!logged) {
					logged = 1;
					slog("font: m_cordStep loc=%d", loc);
				}
			}
			return loc;
		}
		size_t l = strlen(p->unis[i].name);
		if (l > 3 && strcmp(p->unis[i].name + l - 3, "[0]") == 0) {
			char base[48];
			snprintf(base, sizeof(base), "%.*s", (int)(l - 3), p->unis[i].name);
			if (strcmp(base, name) == 0) {
				if (strcmp(name, "m_cordStep") == 0) {
					static int logged;
					if (!logged) {
						logged = 1;
						slog("font: m_cordStep loc=%d", loc);
					}
				}
				return loc;
			}
		}
	}
	for (int i = 0; i < p->nsamplers; i++)
		if (strcmp(p->samplers[i].name, name) == 0)
			return (GLint)p->samplers[i].off;
	if (strcmp(name, "m_cordStep") == 0)
		slog("font: m_cordStep NOT FOUND");
	return -1;
}
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
	if (!params)
		return;
	ShimShader* sh = shader_by_id(shader);
	switch (pname) {
	case GL_COMPILE_STATUS:
		*params = (sh && sh->blob) ? GL_TRUE : GL_FALSE;
		break;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		break;
	default:
		*params = 0;
		break;
	}
}
void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
	if (!params)
		return;
	ShimProgram* p = program_by_id(program);
	switch (pname) {
	case GL_LINK_STATUS:
		*params = (p && p->linked) ? GL_TRUE : GL_FALSE;
		break;
	case GL_VALIDATE_STATUS:
		*params = (p && p->linked) ? GL_TRUE : GL_FALSE;
		break;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		break;
	default:
		*params = 0;
		break;
	}
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
	(void)shader;
	if (bufSize > 0)
		infoLog[0] = '\0';
	if (length)
		*length = 0;
}
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
	(void)program;
	if (bufSize > 0)
		infoLog[0] = '\0';
	if (length)
		*length = 0;
}

static uint8_t* uniform_ptr(GLint location, size_t bytes) {
	ShimProgram* p = program_by_id(g.program);
	if (!p || location < 0)
		return NULL;
	uint8_t* base = NULL;
	uint32_t off = (uint32_t)location;
	if (location >= 0x20000) {
		base = p->cbuf_ps;
		off = (uint32_t)location - 0x20000u;
	} else if (location < 0x10000) {
		base = p->cbuf_vs;
	} else {
		return NULL;
	}
	if (!base || (size_t)off + bytes > p->cbufsize) {
		shim_set_error(GL_INVALID_VALUE);
		return NULL;
	}
	return base + off;
}

void glUniform1f(GLint location, GLfloat v0) {
	uint8_t* d = uniform_ptr(location, 4);
	if (d)
		memcpy(d, &v0, 4);
}
void glUniform1i(GLint location, GLint v0) {
	/* Sampler uniforms select the GL texture unit.  The descriptor set is
	 * built later; remember the requested unit instead of assuming that
	 * sampler index N always uses unit N (the dither sampler uses unit 4,
	 * and PSSL may reorder the sampler table). */
	ShimProgram* p = program_by_id(g.program);
	if (p) {
		for (int i = 0; i < p->nsamplers; i++) {
			if (location == (GLint)p->samplers[i].off) {
				p->sampler_unit[i] = v0;
#if defined(PS4_RENDER_CANARY)
				CANARY_MARK(CANARY_SLOT_SAMPLER);
				if (CANARY_TAKE(CANARY_SLOT_SAMPLER, 32))
					slog("canary: sampler prog=%u idx=%d unit=%d",
					     p->id, i, v0);
#endif
				return;
			}
		}
	}
	uint8_t* d = uniform_ptr(location, 4);
	if (d)
		memcpy(d, &v0, 4);
}
void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
	uint8_t* d = uniform_ptr(location, 8);
	if (d) {
		float v[2] = {v0, v1};
		memcpy(d, v, 8);
	}
}
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
	uint8_t* d = uniform_ptr(location, 12);
	if (d) {
		float v[3] = {v0, v1, v2};
		memcpy(d, v, 12);
	}
}
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	uint8_t* d = uniform_ptr(location, 16);
	if (d) {
		float v[4] = {v0, v1, v2, v3};
		memcpy(d, v, 16);
	}
}
void glUniform4fv(GLint location, GLsizei count, const GLfloat* value) {
	/* m_cordStep is the font atlas inverse size (1/width, 1/height). */
	if (count == 1 && value && value[0] > 0.0f && value[0] < 0.01f &&
	    value[1] > 0.0f && value[1] < 0.01f) {
		static int logged;
		if (!logged) {
			logged = 1;
			slog("font: cordstep loc=%d val=%.8f,%.8f,%.8f,%.8f", location,
			     value[0], value[1], value[2], value[3]);
		}
	}
	uint8_t* d = uniform_ptr(location, (size_t)count * 16);
	if (d)
		memcpy(d, value, (size_t)count * 16);
}
static int yuv_debug_mode(void) {
	static int mode = -1;
	if (mode < 0) {
		mode = 0;
		FILE* f = fopen("/data/kodi-ps4/ps4-yuv-debug", "r");
		if (f) {
			char s[16] = {0};
			if (fgets(s, sizeof(s), f)) {
				switch (s[0]) {
				case 'y': case 'Y': mode = 1; break;
				case 'u': case 'U': mode = 2; break;
				case 'v': case 'V': mode = 3; break;
				case 's': case 'S': mode = 4; break;
				case 'c': case 'C': mode = 5; break;
				default: mode = 0; break;
				}
			}
			fclose(f);
		}
	}
	return mode;
}

/* Debug-only: remap the four sampler descriptor pairs to discover which
 * slot the compiled SRT actually reads for U/V.  Read on every draw so
 * the file can be changed over FTP while the video is playing. */
static int yuv_bind_mode(void) {
	FILE* f = fopen("/data/kodi-ps4/ps4-yuv-bind", "r");
	if (!f)
		return 0;
	char s[16] = {0};
	int mode = 0;
	if (fgets(s, sizeof(s), f) && s[0] >= '0' && s[0] <= '7')
		mode = s[0] - '0';
	fclose(f);
	return mode;
}

static int yuvmat_transpose_enabled(void) {
	static int enabled = -1;
	if (enabled < 0) {
		/* PSSL/psbc keeps Kodi's Matrix4 column-major layout (the GUI
		 * matrices prove it).  Transposing m_yuvmat was an experiment and
		 * produced ochre/white video; default to the original copy. */
		enabled = 0;
		FILE* f = fopen("/data/kodi-ps4/ps4-yuvmat-transpose", "r");
		if (f) {
			int v = 1;
			if (fscanf(f, "%d", &v) == 1)
				enabled = v ? 1 : 0;
			fclose(f);
		}
	}
	return enabled;
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
			const GLfloat* value) {
	(void)transpose;
	uint8_t* d = uniform_ptr(location, (size_t)count * 64);
	if (!d)
		return;

	/* Optional diagnostic: /data/kodi-ps4/ps4-yuvmat-transpose = 1
	 * transposes m_yuvmat.  Default is 0 because PSSL already matches
	 * Kodi's column-major layout; transposing it gives ochre/white. */
	ShimProgram* p = program_by_id(g.program);
	const GLint yuvloc =
	    p && p->has_yuvmat
		? (GLint)(p->yuvmat_off + (p->yuvmat_stage ? 0x20000u : 0u))
		: -1;
	if (p && p->has_yuvmat && location == yuvloc && yuvmat_transpose_enabled()) {
		for (GLsizei i = 0; i < count; i++) {
			const float* m = value + (size_t)i * 16;
			float t[16];
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++)
					t[r * 4 + c] = m[c * 4 + r];
			memcpy(d + (size_t)i * 64, t, 64);
		}
		return;
	}
	memcpy(d, value, (size_t)count * 64);
}
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
			const GLfloat* value) {
	(void)transpose;
	/* std140 mat3 is three vec4s: copy each column padded to 16 bytes. */
	uint8_t* d = uniform_ptr(location, (size_t)count * 48);
	if (!d)
		return;
	for (GLsizei i = 0; i < count; i++) {
		memcpy(d + i * 48 + 0, value + i * 9 + 0, 12);
		memcpy(d + i * 48 + 16, value + i * 9 + 3, 12);
		memcpy(d + i * 48 + 32, value + i * 9 + 6, 12);
	}
}

/* ------------------------------------------------------------------ */
/* draw                                                                */
/* ------------------------------------------------------------------ */

static GnmBlendOp blend_op(GLenum f) {
	switch (f) {
	case GL_ZERO:
		return GNM_BLEND_ZERO;
	case GL_ONE:
		return GNM_BLEND_ONE;
	case GL_SRC_COLOR:
		return GNM_BLEND_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR:
		return GNM_BLEND_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA:
		return GNM_BLEND_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:
		return GNM_BLEND_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:
		return GNM_BLEND_DEST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA:
		return GNM_BLEND_ONE_MINUS_DEST_ALPHA;
	case GL_DST_COLOR:
		return GNM_BLEND_DEST_COLOR;
	case GL_ONE_MINUS_DST_COLOR:
		return GNM_BLEND_ONE_MINUS_DEST_COLOR;
	case GL_SRC_ALPHA_SATURATE:
		return GNM_BLEND_SRC_ALPHA_SATURATE;
	default:
		return GNM_BLEND_ONE;
	}
}

static GnmCombFunc blend_comb(GLenum eq) {
	switch (eq) {
	case GL_FUNC_SUBTRACT:
		return GNM_COMB_SRC_MINUS_DST;
	case GL_FUNC_REVERSE_SUBTRACT:
		return GNM_COMB_DST_MINUS_SRC;
	case 0x8007: /* GL_MIN */
		return GNM_COMB_MIN_DST_SRC;
	case 0x8008: /* GL_MAX */
		return GNM_COMB_MAX_DST_SRC;
	case GL_FUNC_ADD:
	default:
		return GNM_COMB_DST_PLUS_SRC;
	}
}

static GnmDepthCompare depth_compare(GLenum f) {
	switch (f) {
	case GL_NEVER:
		return GNM_DEPTH_COMPARE_NEVER;
	case GL_LESS:
		return GNM_DEPTH_COMPARE_LESS;
	case GL_EQUAL:
		return GNM_DEPTH_COMPARE_EQUAL;
	case GL_LEQUAL:
		return GNM_DEPTH_COMPARE_LESSEQUAL;
	case GL_GREATER:
		return GNM_DEPTH_COMPARE_GREATER;
	case GL_NOTEQUAL:
		return GNM_DEPTH_COMPARE_NOTEQUAL;
	case GL_GEQUAL:
		return GNM_DEPTH_COMPARE_GREATEREQUAL;
	case GL_ALWAYS:
	default:
		return GNM_DEPTH_COMPARE_ALWAYS;
	}
}

static GnmCullMode cull_mode(void) {
	if (!g.cull_face_enabled)
		return GNM_CULL_NONE;
	switch (g.cull_face) {
	case GL_FRONT:
		return GNM_CULL_FRONT;
	case GL_BACK:
		return GNM_CULL_BACK;
	case GL_FRONT_AND_BACK:
		return GNM_CULL_FRONTBACK;
	default:
		return GNM_CULL_NONE;
	}
}

static GnmSampler make_sampler(const ShimTexture* t) {
	GnmSampler s;
	memset(&s, 0, sizeof(s));
	const GLint wraps = t ? t->wraps : GL_CLAMP_TO_EDGE;
	const GLint wrapt = t ? t->wrapt : GL_CLAMP_TO_EDGE;
	s.clampx = wraps == GL_REPEAT ? GNM_TEX_CLAMP_WRAP : GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
	s.clampy = wrapt == GL_REPEAT ? GNM_TEX_CLAMP_WRAP : GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
	const GLint minf = t ? t->minfilter : GL_LINEAR;
	const GLint magf = t ? t->magfilter : GL_LINEAR;
	s.xyminfilter = (minf == GL_NEAREST) ? GNM_FILTER_POINT : GNM_FILTER_BILINEAR;
	s.xymagfilter = (magf == GL_NEAREST) ? GNM_FILTER_POINT : GNM_FILTER_BILINEAR;
	s.mipfilter = GNM_MIPFILTER_NONE;
	s.minlod = 0;
	s.maxlod = 0xfff;
	return s;
}

/* Resolve the index source for an indexed draw.  GNM reads the indices at
 * present time, so the caller must snapshot them (see draw_common); this
 * helper only validates and returns a stable pointer for the scan. */
static int index_source(GLenum type, const void* indices, GLsizei count,
			const uint8_t** out, size_t* avail, size_t* elem) {
	if (g.element_buffer) {
		ShimBuffer* ib = buffer_by_id(g.element_buffer);
		const size_t off = (size_t)(uintptr_t)indices;
		if (!ib || !ib->data || off > ib->size) {
			shim_set_error(GL_INVALID_OPERATION);
			return 0;
		}
		*out = (const uint8_t*)ib->data + off;
		*avail = ib->size - off;
	} else {
		if (!indices) {
			shim_set_error(GL_INVALID_OPERATION);
			return 0;
		}
		*out = (const uint8_t*)indices;
		*avail = (size_t)-1;
	}
	switch (type) {
	case GL_UNSIGNED_BYTE:
		*elem = 1;
		break;
	case GL_UNSIGNED_SHORT:
		*elem = 2;
		break;
	case GL_UNSIGNED_INT:
		*elem = 4;
		break;
	default:
		shim_set_error(GL_INVALID_ENUM);
		return 0;
	}
	if ((size_t)count > *avail / *elem) {
		shim_set_error(GL_INVALID_OPERATION);
		return 0;
	}
	return 1;
}

static int draw_common(GLenum mode, GLint first, GLsizei count, GLenum type,
		       const void* indices, int indexed) {
	if (!g.ready)
		glShimInit(1920, 1080);
	ShimProgram* p = program_by_id(g.program);
	if (!g.ready || !p || !p->linked || count <= 0)
		return 0;

	/* Number of vertices the draw can touch.  For indexed draws this is the
	 * highest index + 1, not the index count: Kodi's GUI uses six indices
	 * per quad, so using the index count as a vertex count would overrun
	 * the VBO, while count/6*4 is wrong for plain triangle lists. */
	const uint8_t* isrc = NULL;
	size_t iavail = 0;
	size_t ielem = 0;
	uint32_t nverts = (uint32_t)count;
	if (indexed) {
		if (!index_source(type, indices, count, &isrc, &iavail, &ielem))
			return 0;
		uint32_t maxidx = 0;
		for (GLsizei i = 0; i < count; i++) {
			uint32_t v = ielem == 1 ? isrc[i]
					 : (ielem == 2 ? ((const uint16_t*)isrc)[i]
						       : ((const uint32_t*)isrc)[i]);
			if (v > maxidx)
				maxidx = v;
		}
		nverts = maxidx + 1;
		if (nverts == 0)
			nverts = 1;
		(void)iavail;
	}

	GnmRenderTarget* colorrt = current_render_target();
	if (!colorrt)
		return 0;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_DRAW);
	if (CANARY_TAKE(CANARY_SLOT_DRAW, 16))
		slog("canary: draw prog=%u mode=0x%x count=%d indexed=%d fbo=%u fb=%d attrs=%d tex=%u,%u,%u,%u",
		     p->id, mode, count, indexed, g.cur_fbo, g.fbidx, p->nattrs,
		     g.texunit[0], g.texunit[1], g.texunit[2], g.texunit[3]);
#endif

	GnmPrimitiveType prim;
	switch (mode) {
	case GL_TRIANGLES:
		prim = GNM_PT_TRILIST;
		break;
	case GL_TRIANGLE_STRIP:
		prim = GNM_PT_TRISTRIP;
		break;
	case GL_TRIANGLE_FAN:
		prim = GNM_PT_TRIFAN;
		break;
	default:
		shim_set_error(GL_INVALID_ENUM);
		return 0;
	}

	/* One vertex view per VS input semantic, snapshotted in the arena. */
	const GnmVertexInputSemantic* sem = gnmVsShaderInputSemanticTable(p->vsh);
	const int nsem = p->vsh->numinputsemantics;
	GnmBuffer* views = arena_alloc(sizeof(GnmBuffer) * (size_t)(nsem ? nsem : 1), 16);
	if (!views)
		return 0;
	for (int i = 0; i < nsem; i++) {
		ShimAttrib* a = sem[i].semantic < MAX_ATTRIBS ? &g.attribs[sem[i].semantic] : NULL;
		ShimBuffer* vb = NULL;
		const uint8_t* vbase = NULL;
		if (a && a->enabled) {
			if (a->client) {
				if (a->offset)
					vbase = (const uint8_t*)(uintptr_t)a->offset;
			} else {
				vb = buffer_by_id(a->buffer);
				if (vb && vb->data)
					vbase = (const uint8_t*)vb->data + a->offset;
			}
		}
		if (!a || !vbase) {
			views[i] = gnmCreateVertexBuffer(NULL, GNM_FMT_R32G32_FLOAT, 0, 0);
			continue;
		}
		const GnmDataFormat* fmt = attrib_format(a);
		if (!fmt) {
			views[i] = gnmCreateVertexBuffer(NULL, GNM_FMT_R32G32_FLOAT, 0, 0);
			continue;
		}
		const uint32_t elembytes = a->type == GL_UNSIGNED_BYTE    ? 1
					   : a->type == GL_UNSIGNED_SHORT ? 2
									  : 4;
		const uint32_t stride =
		    a->stride ? (uint32_t)a->stride : (uint32_t)a->size * elembytes;
		if (stride == 0 || nverts == 0 ||
		    (size_t)nverts > FRAME_ARENA_SIZE / (size_t)stride) {
			views[i] = gnmCreateVertexBuffer(NULL, GNM_FMT_R32G32_FLOAT, 0, 0);
			continue;
		}

		/* Kodi rewrites its vertex caches (font cache, GUI buffer arena)
		 * before the deferred GNM command buffer is submitted.  Reference
		 * the caller's VBO directly would make earlier draws sample the
		 * positions/colors of a later label: exactly the rapid text
		 * flicker seen while moving the selection.  Snapshot every
		 * attribute range into the frame arena, whether it comes from a
		 * client array or a VBO.
		 *
		 * Only the attribute's own bytes are needed on the last vertex;
		 * copying nverts*stride from an attribute with a non-zero offset
		 * would read past the end of the VBO and reject valid draws. */
		const uint32_t attr_bytes = (uint32_t)a->size * elembytes;
		const size_t copy_bytes =
		    nverts > 0 ? (size_t)(nverts - 1) * stride + attr_bytes : 0;
		/* Reserve the full stride for the last vertex so the GNM fetch
		 * unit never reads past the snapshot, but only copy the attribute
		 * bytes that actually exist in the source (an attribute with a
		 * non-zero offset does not own the bytes after its last component). */
		const size_t alloc_bytes = (size_t)nverts * stride;
		const uint8_t* src = vbase + (size_t)first * stride;
		if (vb) {
			const size_t rel = (size_t)a->offset + (size_t)first * stride;
			if (rel > vb->size || copy_bytes > vb->size - rel) {
				static int vbo_range_logged;
				if (!vbo_range_logged) {
					vbo_range_logged = 1;
					slog("vertex: VBO range overflow attr=%d off=%zu first=%d "
					     "verts=%u stride=%u size=%zu",
					     i, (size_t)a->offset, first, nverts, stride,
					     vb->size);
				}
				views[i] =
				    gnmCreateVertexBuffer(NULL, GNM_FMT_R32G32_FLOAT, 0, 0);
				continue;
			}
		}
		uint8_t* snapshot = arena_alloc(alloc_bytes, 16);
		if (!snapshot) {
			shim_set_error(GL_OUT_OF_MEMORY);
			views[i] = gnmCreateVertexBuffer(NULL, GNM_FMT_R32G32_FLOAT, 0, 0);
			continue;
		}
		memcpy(snapshot, src, copy_bytes);
		if (a->client) {
			static int client_attr_logged;
			if (!client_attr_logged) {
				client_attr_logged = 1;
				slog("vertex: client copy nverts=%u stride=%u", nverts, stride);
			}
		}

		/* Kodi's GLES font path is the only GUI path that uploads texture
		 * coordinates as GL_UNSIGNED_SHORT (texels).  By default expand
		 * them to float on the CPU; PS4_GL_NATIVE_UV=1 lets the GNM fetch
		 * unit do the u16->float conversion instead. */
		const bool font_uv =
		    a->type == GL_UNSIGNED_SHORT && a->size == 2 && p->nattrs == 4;
		if (font_uv && !g.native_uv) {
			float* tmp = arena_alloc((size_t)nverts * 2 * sizeof(float), 4);
			if (tmp) {
				uint32_t v = 0;
#if defined(__SSE4_1__)
				/* Two vertices per iteration: 4 u16 -> 4 u32 -> 4 f32.
				 * The UVs are interleaved in the vertex buffer (stride
				 * bytes apart), so gather the two 32-bit UV pairs; a
				 * single 64-bit load would read padding instead of the
				 * second vertex's UV and collapse half the glyph quads. */
				for (; v + 2 <= nverts; v += 2) {
					uint32_t uv0, uv1;
					memcpy(&uv0, snapshot + (size_t)v * stride, 4);
					memcpy(&uv1, snapshot + (size_t)(v + 1) * stride, 4);
					const __m128i uv = _mm_set_epi32(0, 0, (int)uv1, (int)uv0);
					const __m128 f =
					    _mm_cvtepi32_ps(_mm_cvtepu16_epi32(uv));
					_mm_storeu_ps(tmp + (size_t)v * 2, f);
				}
#endif
				for (; v < nverts; v++) {
					const uint16_t* uv =
					    (const uint16_t*)(snapshot + (size_t)v * stride);
					tmp[v * 2 + 0] = (float)uv[0];
					tmp[v * 2 + 1] = (float)uv[1];
				}
				static int logged;
				if (!logged) {
					const uint16_t* firstuv = (const uint16_t*)snapshot;
					logged = 1;
					slog("font: u16->float nverts=%u first=%u,%u stride=%u count=%u",
					     nverts, firstuv[0], firstuv[1], stride,
					     (unsigned)count);
				}
				static int fontdraw_logged;
				if (g_font_dump_enabled && fontdraw_logged < 600) {
					ShimTexture* ft = texture_by_id(g.texunit[0]);
					float cs0 = -1.0f, cs1 = -1.0f;
					int cs_found = 0;
					for (int u = 0; u < p->nunis; u++) {
						if (strcmp(p->unis[u].name, "m_cordStep") == 0 &&
						    p->unis[u].stage == 0) {
							const float* cs =
							    (const float*)(p->cbuf_vs + p->unis[u].off);
							cs0 = cs[0];
							cs1 = cs[1];
							cs_found = 1;
							break;
						}
					}
					slog("fontdraw: prog=%u attrs=%d unis=%d samp=%d "
					     "texid=%u texw=%d texh=%d texbase=%p texpitch=%u "
					     "dyn=%d tile=0x%x mtype=%u/%u/%u "
					     "cordstep=%.8f,%.8f found=%d nverts=%u",
					     p->id, p->nattrs, p->nunis, p->nsamplers,
					     g.texunit[0], ft ? ft->width : 0,
					     ft ? ft->height : 0,
					     ft ? gnmTexGetBaseAddress(&ft->tex) : NULL,
					     ft ? gnmTexGetPitch(&ft->tex) : 0,
					     ft ? ft->dynamic : 0,
					     ft ? ft->tex.tilingindex : 0,
					     ft ? ft->tex.mtype_l2 : 0,
					     ft ? ft->tex.mtype0 : 0,
					     ft ? ft->tex.mtype2 : 0, cs0, cs1, cs_found,
					     nverts);
					fontdraw_logged++;
				}
				views[i] = gnmCreateVertexBuffer(tmp, GNM_FMT_R32G32_FLOAT, 8,
								 nverts);
				continue;
			}
		}
		if (font_uv && g.native_uv) {
			static int native_uv_logged;
			if (!native_uv_logged) {
				native_uv_logged = 1;
				slog("font: native u16 USCALED fetch nverts=%u count=%u",
				     nverts, (unsigned)count);
			}
		}
		const uint32_t view_count = indexed ? nverts : (uint32_t)count;
		views[i] = gnmCreateVertexBuffer(snapshot, *fmt, stride, view_count);
	}

	/* Uniform snapshots: the GPU executes at present time, after later draws
	 * may have overwritten the program's canonical buffers. */
	const size_t vs_size = p->vs_cbuf_size ? p->vs_cbuf_size : 64;
	const size_t ps_size = p->ps_cbuf_size ? p->ps_cbuf_size : 64;
	uint8_t* vsu = arena_alloc(vs_size, 16);
	uint8_t* psu = arena_alloc(ps_size, 16);
	if (!vsu || !psu)
		return 0;
	memcpy(vsu, p->cbuf_vs, vs_size);
	memcpy(psu, p->cbuf_ps, ps_size);

	/* One-shot forensic dump for the YUV program: which textures are
	 * actually bound to units 0..3, their pixel statistics, the attribute
	 * locations and the UV/matrix uniforms Kodi uploaded. */
	if (p->nsamplers >= 3) {
		static int yuv_draw_logged;
		if (yuv_draw_logged < 4) {
			yuv_draw_logged++;
			slog("yuvdraw: prog=%u attrs=%d unis=%d samplers=%d units=%u,%u,%u,%u",
			     p->id, p->nattrs, p->nunis, p->nsamplers,
			     g.texunit[0], g.texunit[1], g.texunit[2], g.texunit[3]);
			slog("yuvdraw: sampler_units=%d,%d,%d,%d",
			     p->nsamplers > 0 ? p->sampler_unit[0] : -1,
			     p->nsamplers > 1 ? p->sampler_unit[1] : -1,
			     p->nsamplers > 2 ? p->sampler_unit[2] : -1,
			     p->nsamplers > 3 ? p->sampler_unit[3] : -1);
			for (int i = 0; i < p->nattrs; i++)
				slog("yuvdraw: attr[%d] name=%s loc=%d", i, p->attrs[i].name,
				     p->attrs[i].loc);
			for (int i = 0; i < 4; i++) {
				ShimTexture* t = texture_by_id(g.texunit[i]);
				slog("yuvdraw: unit%d tex=%u %dx%d bpp=%d base=%p pixels=%p pitch=%u tile=0x%x mtype=%u/%u/%u", i,
				     t ? t->id : 0, t ? t->width : 0, t ? t->height : 0,
				     t ? t->bpp : 0,
				     t ? gnmTexGetBaseAddress(&t->tex) : NULL,
				     t ? t->pixels : NULL,
				     t ? gnmTexGetPitch(&t->tex) : 0,
				     t ? t->tex.tilingindex : 0,
				     t ? t->tex.mtype_l2 : 0,
				     t ? t->tex.mtype0 : 0,
				     t ? t->tex.mtype2 : 0);
				if (!t || !t->pixels || t->bpp != 1 || t->width <= 0 ||
				    t->height <= 0)
					continue;
				const size_t pitch = ((size_t)t->width + 63) & ~(size_t)63;
				unsigned long long sum = 0;
				size_t count = 0;
				int mn = 255, mx = 0;
				for (int yy = 0; yy < t->height; yy++) {
					const uint8_t* row = t->pixels + (size_t)yy * pitch;
					for (int xx = 0; xx < t->width; xx++) {
						const int v = row[xx];
						sum += (unsigned)v;
						count++;
						if (v < mn)
							mn = v;
						if (v > mx)
							mx = v;
					}
				}
				const uint8_t* mid = t->pixels + (size_t)(t->height / 2) * pitch;
				slog("yuvdraw: unit%d stat min=%d max=%d mean=%llu first=%02x,%02x,%02x,%02x mid=%02x,%02x,%02x,%02x",
				     i, mn, mx, count ? sum / count : 0, t->pixels[0],
				     t->pixels[1], t->pixels[2], t->pixels[3], mid[0], mid[1],
				     mid[2], mid[3]);
			}
			for (int u = 0; u < p->nunis; u++) {
				const char* n = p->unis[u].name;
				if (strcmp(n, "m_yuvmat") == 0) {
					const float* m = (const float*)((p->unis[u].stage ? p->cbuf_ps
											       : p->cbuf_vs) +
									p->unis[u].off);
					slog("yuvdraw: yuvmat=%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
					     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
					     m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
				} else if (strncmp(n, "m_texRect", 9) == 0 || strcmp(n, "m_step") == 0 ||
					   strcmp(n, "m_alpha") == 0 || strcmp(n, "m_gammaSrc") == 0 ||
					   strcmp(n, "m_gammaDstInv") == 0) {
					const float* v = (const float*)((p->unis[u].stage ? p->cbuf_ps
											   : p->cbuf_vs) +
									p->unis[u].off);
					slog("yuvdraw: uni %s=%g %g %g %g", n, v[0], v[1], v[2],
					     v[3]);
				}
			}
		}
	}

	/* Kodi's font path passes m_cordStep = 1/atlas size.  If the value does
	 * not match the texture actually bound, the glyph UVs are scaled wrong
	 * and text turns into glyphs from other positions.  The shim knows the
	 * real atlas dimensions, so normalise it here for the font program. */
	if (p->nattrs == 4 && p->nsamplers == 1 && p->has_ps_res) {
		ShimTexture* ft = texture_by_id(g.texunit[0]);
		if (ft && ft->width > 0 && ft->height > 0) {
			for (int u = 0; u < p->nunis; u++) {
				if (strcmp(p->unis[u].name, "m_cordStep") == 0) {
					float step[4] = {1.0f / (float)ft->width,
							 1.0f / (float)ft->height, 1.0f, 1.0f};
					uint8_t* dst = p->unis[u].stage ? psu + p->unis[u].off
								       : vsu + p->unis[u].off;
					static int cord_override_logged;
					if (g_font_dump_enabled && cord_override_logged < 5) {
						const float* orig =
						    (const float*)((p->unis[u].stage ? p->cbuf_ps
										    : p->cbuf_vs) +
								   p->unis[u].off);
						slog("fontdraw: cordstep override orig=%.8f,%.8f "
						     "new=%.8f,%.8f",
						     orig[0], orig[1], step[0], step[1]);
						cord_override_logged++;
					}
					memcpy(dst, step, 16);
					break;
				}
			}
		}
	}

	/* Debug translator for the video canvas only: override m_yuvmat so the
	 * Y/U/V planes can be inspected independently.  Controlled by
	 * /data/kodi-ps4/ps4-yuv-debug: y, u, v, s (swap U/V), c (U=R,V=G,Y=B). */
	const int yuvdbg = p->has_yuvmat ? yuv_debug_mode() : 0;
	if (yuvdbg) {
		uint8_t* base = p->yuvmat_stage ? psu : vsu;
		const size_t limit = p->yuvmat_stage ? ps_size : vs_size;
		if (base && (size_t)p->yuvmat_off + 64 <= limit) {
			float m[16] = {0};
			m[15] = 1.0f;
			if (yuvdbg == 1) {
				m[0] = m[1] = m[2] = 1.0f; /* Y -> RGB */
			} else if (yuvdbg == 2) {
				m[4] = m[5] = m[6] = 1.0f; /* U -> RGB */
			} else if (yuvdbg == 3) {
				m[8] = m[9] = m[10] = 1.0f; /* V -> RGB */
			} else if (yuvdbg == 4) {
				const float* src = (const float*)((p->yuvmat_stage ? p->cbuf_ps
								   : p->cbuf_vs) + p->yuvmat_off);
				memcpy(m, src, 64);
				for (int r = 0; r < 4; r++) {
					float t = m[4 + r];
					m[4 + r] = m[8 + r];
					m[8 + r] = t;
				}
			} else if (yuvdbg == 5) {
				m[0] = 0; m[1] = 0; m[2] = 1; /* Y -> B */
				m[4] = 1; m[5] = 0; m[6] = 0; /* U -> R */
				m[8] = 0; m[9] = 1; m[10] = 0; /* V -> G */
			}
			memcpy(base + p->yuvmat_off, m, 64);
			static int dbg_logged;
			if (dbg_logged < 4) {
				slog("yuvdraw: debug matrix mode=%d", yuvdbg);
				dbg_logged++;
			}
		}
	}

	PsDescSetPair* psdesc = NULL;
	void* desc_base = NULL;
	int bindmode = 0;
	if (p->has_ps_res) {
		/* The PSSL SRT can index sampler slots beyond the number of
		 * declared sampler blocks (the YV12 shader reads Y from slot 4
		 * while only four blocks are reflected).  Size the table to the
		 * hardware maximum and mirror Y into the unused tail so an
		 * out-of-range slot reads luma instead of the next arena
		 * allocation. */
		const int nsamplers =
		    p->nsamplers > 0 ? (p->nsamplers > 8 ? 8 : p->nsamplers) : 1;
		/* The 0x100 header was a wrong hypothesis: the shader read a
		 * zeroed table and the canvas became solid green.  Keep the
		 * 8-pair table at offset 0 (the fix35 layout) while the
		 * 2-sampler NV12 path is implemented. */
		const size_t desc_header = 0;
		desc_base = arena_alloc(desc_header + sizeof(PsDescSetPair) * 8, 256);
		if (!desc_base)
			return 0;
		psdesc = (PsDescSetPair*)((uint8_t*)desc_base + desc_header);
		for (int i = 0; i < 8; i++) {
			/* Extra slots are UV (not Y): the compiled SRT may put
			 * m_sampU on slot 3, and an RGB8 luma texture there
			 * returns alpha=1.0 -> V=1 -> red canvas. */
			const int src = i < nsamplers ? i : (nsamplers > 1 ? 1 : 0);
			int unit = p->sampler_unit[src];
			if (unit < 0 || unit > 7)
				unit = src;
			ShimTexture* t = texture_by_id(g.texunit[unit]);
			psdesc[i].texture = t ? t->tex : (GnmTexture){0};
			psdesc[i].sampler = make_sampler(t);
		}
		/* The YV12 PSSL blob has four sampler *blocks* (Y, U, V and the
		 * dither sampler) but the compiled SRT may bind m_sampV to the
		 * last slot.  When the dither texture is not bound, the V pair
		 * in the reflection table no longer matches the slot the shader
		 * actually fetches, and chroma reads 0 (red/blue frames render
		 * green).  Mirror V into the unused trailing slots so the
		 * chroma survives either slot order.  A real dither texture is
		 * left untouched. */
		if (nsamplers >= 4) {
			int vidx = -1;
			int didx = -1;
			for (int i = 0; i < nsamplers; i++) {
				if (strcmp(p->samplers[i].name, "m_sampV") == 0)
					vidx = i;
				else if (strcmp(p->samplers[i].name, "m_dither") == 0)
					didx = i;
			}
			if (vidx >= 0 && didx >= 0 && vidx < didx) {
				const int vunit = p->sampler_unit[vidx];
				const int dunit = p->sampler_unit[didx];
				const ShimTexture* vt = (vunit >= 0 && vunit <= 7)
							    ? texture_by_id(g.texunit[vunit])
							    : NULL;
				if (vt && vt->width > 0 &&
				    (dunit < 0 || dunit > 7 || g.texunit[dunit] == 0)) {
					for (int i = didx; i < nsamplers; i++)
						psdesc[i] = psdesc[vidx];
					static int vmirror_logged;
					if (vmirror_logged < 4) {
						vmirror_logged++;
						slog("yuvdraw: V mirrored into psdesc[%d..%d] (dither unbound)",
						     didx, nsamplers - 1);
					}
				}
			}
		}
		/* Debug-only descriptor permutation; see yuv_bind_mode(). */
		bindmode = p->nsamplers >= 3 ? yuv_bind_mode() : 0;
		if (bindmode) {
			PsDescSetPair orig[8];
			memcpy(orig, psdesc, sizeof(orig));
			for (int i = 0; i < 8; i++) {
				switch (bindmode) {
				case 1: psdesc[i] = orig[0]; break;
				case 2: psdesc[i] = orig[1]; break;
				case 3: psdesc[i] = orig[2]; break;
				case 4:
					psdesc[i] = i < nsamplers ? orig[(i + 1) % nsamplers]
								  : orig[0];
					break;
				case 5:
					psdesc[i] = i < nsamplers
							? orig[(i + nsamplers - 1) % nsamplers]
							: orig[0];
					break;
				case 6:
					psdesc[i] = orig[i == 1 ? 2 : (i == 2 ? 1 : i)];
					break;
				default: break;
				}
			}
			static int bindmode_logged = -1;
			if (bindmode_logged != bindmode) {
				bindmode_logged = bindmode;
				slog("yuvdraw: bind mode=%d (psdesc remapped)", bindmode);
			}
		}
		static int psdesc_logged;
		if (p->nsamplers >= 3 && psdesc_logged < 4) {
			psdesc_logged++;
			slog("yuvdraw: desc_base=%p align=%u psdesc=%p res_slot=%u const_slot=%u has_res=%d has_const=%d",
			     desc_base, (unsigned)((uintptr_t)desc_base & 0xffu), psdesc,
			     p->ps_res_slot, p->ps_const_slot, p->has_ps_res, p->has_ps_const);
			for (int i = 0; i < 8; i++) {
				int unit = p->sampler_unit[i < p->nsamplers ? i : 0];
				if (unit < 0 || unit > 7)
					unit = i < p->nsamplers ? i : 0;
				slog("yuvdraw: psdesc[%d] name=%s unit=%d tex=%u",
				     i, i < p->nsamplers ? p->samplers[i].name : "extra",
				     unit, g.texunit[unit]);
			}
		}
	}

	const GnmPrimitiveSetup primsetup = {
	    .cullmode = cull_mode(),
	    .frontface = g.front_face == GL_CW ? GNM_FACE_CW : GNM_FACE_CCW,
	    .frontmode = GNM_FILL_SOLID,
	    .backmode = GNM_FILL_SOLID,
	    .provokemode = GNM_PROVOKINGVTX_FIRST,
	};
	const GnmBlendControl blendctrl = {
	    .blendenabled = g.blend ? true : false,
	    .colorfunc = blend_comb(g.blend_equation),
	    .colorsrcmult = blend_op(g.blend_src),
	    .colordstmult = blend_op(g.blend_dst),
	    .alphafunc = blend_comb(g.blend_equation_a),
	    .alphasrcmult = blend_op(g.blend_src_a),
	    .alphadstmult = blend_op(g.blend_dst_a),
	    .separatealphaenable = true,
	};
	const GnmDbRenderControl dbrenderctrl = {0};
	const GnmDepthStencilControl depthstencilctrl = {
	    .zwrite = g.depth_mask != 0,
	    .zfunc = depth_compare(g.depth_func),
	    .depthenable = g.depth_test != 0,
	};

	if (!g.draw.valid) {
		ShimFramebuffer* cfb = framebuffer_by_id(g.cur_fbo);
		gnmDrawCmdSetPrimitiveSetup(&g.cmd, &primsetup);
		gnmDrawCmdSetDbRenderControl(&g.cmd, &dbrenderctrl);
		gnmDrawCmdSetDepthStencilControl(&g.cmd, &depthstencilctrl);
		gnmDrawCmdSetRenderTarget(&g.cmd, 0, colorrt);
		gnmDrawCmdSetDepthRenderTarget(&g.cmd, cfb ? NULL : &g.depthtarget);
		gnmDrawCmdSetRenderTargetMask(&g.cmd, (uint32_t)g.colormask);
		setupviewport(&g.cmd, (uint32_t)g.vp[0], (uint32_t)g.vp[1],
			      (uint32_t)(g.vp[0] + g.vp[2]), (uint32_t)(g.vp[1] + g.vp[3]),
			      0.5f, 0.5f);
		if (cfb) {
			static int fbo_draw_logged;
			if (fbo_draw_logged < 8) {
				fbo_draw_logged++;
				slog("fbo draw: fbo=%u prog=%u vp=%d,%d,%d,%d blend=%d",
				     cfb->id, p->id, g.vp[0], g.vp[1], g.vp[2], g.vp[3],
				     g.blend);
			}
		}
		memcpy(g.draw.vp, g.vp, sizeof(g.vp));
		g.draw.valid = 1;
		g.draw.fbidx = g.fbidx;
		g.draw.colormask = g.colormask;
		g.draw.program = 0;
		g.draw.blend = -1;
		g.draw.scissor_test = -1;
		g.draw.cullmode = primsetup.cullmode;
		g.draw.frontface = primsetup.frontface;
		g.draw.depth_test = g.depth_test;
		g.draw.depth_mask = g.depth_mask;
		g.draw.depth_func = g.depth_func;
		g.draw.prim = (GnmPrimitiveType)-1;
		g.draw.indexed = -1;
		g.draw.index_type = 0;
	}
	if (g.draw.fbidx != g.fbidx) {
		gnmDrawCmdSetRenderTarget(&g.cmd, 0, colorrt);
		g.draw.fbidx = g.fbidx;
	}
	if (g.draw.colormask != g.colormask) {
		gnmDrawCmdSetRenderTargetMask(&g.cmd, (uint32_t)g.colormask);
		g.draw.colormask = g.colormask;
	}
	if (memcmp(g.draw.vp, g.vp, sizeof(g.vp)) != 0) {
		setupviewport(&g.cmd, (uint32_t)g.vp[0], (uint32_t)g.vp[1],
			      (uint32_t)(g.vp[0] + g.vp[2]), (uint32_t)(g.vp[1] + g.vp[3]),
			      0.5f, 0.5f);
		memcpy(g.draw.vp, g.vp, sizeof(g.vp));
		g.draw.scissor_test = -1;
	}
	if (g.draw.cullmode != primsetup.cullmode || g.draw.frontface != primsetup.frontface) {
		gnmDrawCmdSetPrimitiveSetup(&g.cmd, &primsetup);
		g.draw.cullmode = primsetup.cullmode;
		g.draw.frontface = primsetup.frontface;
	}
	if (g.draw.depth_test != g.depth_test || g.draw.depth_mask != g.depth_mask ||
	    g.draw.depth_func != g.depth_func) {
		gnmDrawCmdSetDepthStencilControl(&g.cmd, &depthstencilctrl);
		g.draw.depth_test = g.depth_test;
		g.draw.depth_mask = g.depth_mask;
		g.draw.depth_func = g.depth_func;
	}
	if (g.draw.blend != g.blend || g.draw.blend_src != g.blend_src ||
	    g.draw.blend_dst != g.blend_dst || g.draw.blend_src_a != g.blend_src_a ||
	    g.draw.blend_dst_a != g.blend_dst_a ||
	    g.draw.blend_equation != g.blend_equation ||
	    g.draw.blend_equation_a != g.blend_equation_a) {
		gnmDrawCmdSetBlendControl(&g.cmd, 0, &blendctrl);
		g.draw.blend = g.blend;
		g.draw.blend_src = g.blend_src;
		g.draw.blend_dst = g.blend_dst;
		g.draw.blend_src_a = g.blend_src_a;
		g.draw.blend_dst_a = g.blend_dst_a;
		g.draw.blend_equation = g.blend_equation;
		g.draw.blend_equation_a = g.blend_equation_a;
	}

	apply_screen_scissor(false);

	if (g.draw.program != p->id) {
		gnmDrawCmdSetVsShader(&g.cmd, &p->vsh->registers, 0);
		gnmDrawCmdSetPsShader(&g.cmd, &p->psh->registers);
		gnmDrawCmdSetPsInputUsage(&g.cmd, gnmVsShaderExportSemanticTable(p->vsh),
					  p->vsh->numexportsemantics,
					  gnmPsShaderInputSemanticTable(p->psh),
					  p->psh->numinputsemantics);
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_VS, p->vs_fetch_slot, p->fetchmem);
		g.draw.program = p->id;
	}

	gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_VS, p->vs_vb_slot, views);
	if (p->has_vs_const)
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_VS, p->vs_const_slot, vsu);
	if (p->has_ps_const)
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_PS, p->ps_const_slot, psu);
	if (psdesc)
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_PS, p->ps_res_slot, desc_base);
	/* Debug-only: swap the resource-table and constant-buffer slots. */
	if (bindmode == 7 && psdesc && p->has_ps_const) {
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_PS, p->ps_const_slot, desc_base);
		gnmDrawCmdSetPointerUserData(&g.cmd, GNM_STAGE_PS, p->ps_res_slot, psu);
	}

	if (g.draw.prim != prim) {
		gnmDrawCmdSetPrimitiveType(&g.cmd, prim);
		g.draw.prim = prim;
	}
	if (indexed) {
		const uint8_t* src = isrc;
		const size_t elem = ielem;
		GLenum draw_type = type;
		if (type == GL_UNSIGNED_BYTE) {
			/* GNM has 16/32-bit indices only.  Expand byte indices
			 * into the frame arena before the command is submitted. */
			static int u8_index_logged;
			if (!u8_index_logged) {
				u8_index_logged = 1;
				slog("index: u8->u16 count=%d", count);
			}
			if ((size_t)count > FRAME_ARENA_SIZE / sizeof(uint16_t)) {
				shim_set_error(GL_OUT_OF_MEMORY);
				return 0;
			}
			uint16_t* idx16 =
			    arena_alloc((size_t)count * sizeof(uint16_t), 4);
			if (!idx16) {
				shim_set_error(GL_OUT_OF_MEMORY);
				return 0;
			}
			for (GLsizei i = 0; i < count; i++)
				idx16[i] = src[i];
			src = (const uint8_t*)idx16;
			draw_type = GL_UNSIGNED_SHORT;
		} else {
			/* Both client arrays and bound GL_ELEMENT_ARRAY_BUFFER
			 * data are read by GNM at present time.  Kodi rewrites its
			 * buffer arenas before that, so snapshot the indices. */
			if ((size_t)count > FRAME_ARENA_SIZE / elem) {
				shim_set_error(GL_OUT_OF_MEMORY);
				return 0;
			}
			uint8_t* copy = arena_alloc((size_t)count * elem, 4);
			if (!copy) {
				shim_set_error(GL_OUT_OF_MEMORY);
				return 0;
			}
			memcpy(copy, src, (size_t)count * elem);
			src = copy;
		}
		if (p->nattrs == 4 && p->nsamplers == 1 && p->has_ps_res &&
		    type == GL_UNSIGNED_SHORT) {
			static int fontindex_logged;
			if (g_font_dump_enabled && fontindex_logged < 20) {
				const uint16_t* idx = (const uint16_t*)src;
				slog("fontdraw: indices=%u,%u,%u,%u,%u,%u count=%u off=%zu",
				     idx[0], idx[1], idx[2], idx[3], idx[4], idx[5],
				     (unsigned)count, (size_t)(uintptr_t)indices);
				fontindex_logged++;
			}
		}
		if (g.draw.indexed != 1 || g.draw.index_type != draw_type) {
			gnmDrawCmdSetIndexSize(&g.cmd,
					       draw_type == GL_UNSIGNED_INT ? GNM_INDEX_32
									    : GNM_INDEX_16,
					       GNM_POLICY_LRU);
			g.draw.indexed = 1;
			g.draw.index_type = draw_type;
		}
		gnmDrawCmdDrawIndex(&g.cmd, (uint32_t)count, src);
	} else {
		g.draw.indexed = 0;
		gnmDrawCmdDrawIndexAuto(&g.cmd, (uint32_t)count);
	}
	g.draw.draws++;
	return 1;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
	const uint64_t draw_t0 = sceKernelGetProcessTime();
	draw_common(mode, first, count, 0, NULL, 0);
	g_perf_draw_us += sceKernelGetProcessTime() - draw_t0;
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
	const uint64_t draw_t0 = sceKernelGetProcessTime();
	draw_common(mode, 0, count, type, indices, 1);
	g_perf_draw_us += sceKernelGetProcessTime() - draw_t0;
}

/* ------------------------------------------------------------------ */
/* stubs for the GLES3 surface API Kodi links but the GUI barely uses   */
/* ------------------------------------------------------------------ */

void glGenVertexArrays(GLsizei n, GLuint* arrays) {
	for (GLsizei i = 0; i < n; i++)
		arrays[i] = (GLuint)(i + 1);
}
void glBindVertexArray(GLuint array) { (void)array; }
void glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
	(void)n;
	(void)arrays;
}
/* Rebuild the attached GL texture as a GNM color surface: the render target
 * and the texture must share base address, pitch and tiling, or the final
 * video pass samples a different memory layout (solid/garbled output). */
static int fbo_make_color_texture(ShimFramebuffer* fb, ShimTexture* t) {
	GpaSurfaceProperties props = {0};
	if (gpaFindOptimalSurface(&props, GPA_SURFACE_COLORDISPLAY, 32, 1, false,
				  gnmGpuMode()) != GPA_ERR_OK) {
		slog("fbo: no optimal surface for %dx%d", t->width, t->height);
		return 0;
	}

	const GnmRenderTargetCreateInfo rci = {
	    .colorfmt = GNM_FMT_R8G8B8A8_UNORM,
	    .width = (uint32_t)t->width,
	    .height = (uint32_t)t->height,
	    .pitch = 0,
	    .numslices = 1,
	    .numsamples = 1,
	    .numfragments = 1,
	    .colortilemodehint = props.tilemode,
	    .mingpumode = gnmGpuMode(),
	};
	GnmRenderTarget rt = {0};
	if (gnmCreateRenderTarget(&rt, &rci) != GNM_ERROR_OK) {
		slog("fbo: create render target %dx%d failed", t->width, t->height);
		return 0;
	}
	uint64_t rtsize = 0;
	uint32_t rtalign = 0;
	if (gnmRtCalcByteSize(&rtsize, &rtalign, &rt) != GNM_ERROR_OK) {
		slog("fbo: render target size failed");
		return 0;
	}

	const uint32_t rtpitch = gnmRtGetPitch(&rt);
	const uint32_t rtrows = gnmRtGetSliceSize(&rt);
	const GnmTextureCreateInfo tci = {
	    .format = GNM_FMT_R8G8B8A8_UNORM,
	    .texturetype = GNM_TEXTURE_2D,
	    .width = rtpitch,
	    .height = rtrows,
	    .depth = 1,
	    .pitch = rtpitch,
	    .nummiplevels = 1,
	    .numslices = 1,
	    .numfragments = 1,
	    .tilemodehint = (GnmTileMode)rt.attrib.tilemode_index,
	    .mingpumode = gnmGpuMode(),
	};
	GnmTexture tex = {0};
	if (gnmCreateTexture(&tex, &tci) != GNM_ERROR_OK) {
		slog("fbo: create texture %ux%u failed", rtpitch, rtrows);
		return 0;
	}
	uint64_t texsize = 0;
	uint32_t texalign = 0;
	if (gnmTexCalcByteSize(&texsize, &texalign, &tex) != GNM_ERROR_OK) {
		slog("fbo: texture size failed");
		return 0;
	}
	const uint64_t size = texsize > rtsize ? texsize : rtsize;
	uint32_t align = rtalign > texalign ? rtalign : texalign;
	if (align == 0)
		align = GNM_ALIGNMENT_SHADER_BYTES;
	const size_t alloc_bytes = (size_t)size + sizeof(uint64_t) + ((size_t)size & 1u);
	void* mem = memalloc_alloc(&g.mem, alloc_bytes, align);
	if (!mem) {
		slog("fbo: alloc %llu failed", (unsigned long long)size);
		return 0;
	}
	memset(mem, 0, alloc_bytes);
	gnmRtSetBaseAddr(&rt, mem);
	gnmTexSetBaseAddress(&tex, mem);
	gnmTexSetWidth(&tex, (uint32_t)t->width);
	gnmTexSetHeight(&tex, (uint32_t)t->height);
	gnmTexSetMemoryType(&tex, GNM_MEMORY_CACHE_COHERENT, true);

	/* The CPU texel store is replaced by the GPU color surface; the old
	 * allocation is freed once the current frame is done with it. */
	if (t->pixels) {
		texture_canary_check(t, "fbo-replace");
		garbage_add(t->pixels);
	}
	t->pixels = mem;
	t->bytes = (size_t)size;
	texture_canary_write(t);
	t->bpp = 4;
	t->tex = tex;
	t->dynamic = 0;
	g.textures_dirty = 1;

	fb->texid = t->id;
	fb->rt = rt;
	fb->rt_valid = 1;
	fb->width = t->width;
	fb->height = t->height;
	slog("fbo: tex=%u %dx%d rt tile=%u pitch=%u rows=%u tex tile=%u pitch=%u rows=%u "
	     "size=%llu mem=%p",
	     t->id, t->width, t->height, rt.attrib.tilemode_index, rtpitch, rtrows,
	     tex.tilingindex, gnmTexGetPitch(&tex), gnmTexGetHeight(&tex),
	     (unsigned long long)size, mem);
	return 1;
}

void glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = 0;
		for (int j = 0; j < MAX_FRAMEBUFFERS; j++) {
			if (!g.framebuffers[j].used) {
				memset(&g.framebuffers[j], 0, sizeof(ShimFramebuffer));
				g.framebuffers[j].used = 1;
				g.framebuffers[j].id = (GLuint)j + 1;
				id = g.framebuffers[j].id;
				break;
			}
		}
		framebuffers[i] = id;
	}
}
void glBindFramebuffer(GLenum target, GLuint framebuffer) {
	(void)target;
	if (!g.ready)
		return;
	if (framebuffer != 0 && !framebuffer_by_id(framebuffer)) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	/* Make the color writes visible to the sampler before the next draw
	 * samples the attached texture. */
	if (g.cur_fbo != 0 && framebuffer == 0)
		gnmDrawCmdWaitGraphicsWrite(&g.cmd, GNM_ACQUIRE_TARGET_CB0);
	g.cur_fbo = framebuffer;
	/* The color target is part of the cached GNM state. */
	g.draw.valid = 0;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_FBO);
	if (CANARY_TAKE(CANARY_SLOT_FBO, 12))
		slog("canary: bind fbo=%u cur=%u", framebuffer, g.cur_fbo);
#endif
}
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
	for (GLsizei i = 0; i < n; i++) {
		ShimFramebuffer* fb = framebuffer_by_id(framebuffers[i]);
		if (!fb)
			continue;
		if (g.cur_fbo == fb->id)
			g.cur_fbo = 0;
		/* The memory belongs to the attached texture (t->pixels);
		 * glDeleteTextures() frees it exactly once. */
		memset(fb, 0, sizeof(*fb));
		g.draw.valid = 0;
	}
}
GLenum glCheckFramebufferStatus(GLenum target) {
	(void)target;
	ShimFramebuffer* fb = framebuffer_by_id(g.cur_fbo);
	if (!fb)
		return GL_FRAMEBUFFER_COMPLETE;
	return fb->rt_valid ? GL_FRAMEBUFFER_COMPLETE
			    : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}
void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
	for (GLsizei i = 0; i < n; i++)
		renderbuffers[i] = (GLuint)(i + 1);
}
void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
	(void)target;
	(void)renderbuffer;
}
void glDeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) {
	(void)n;
	(void)renderbuffers;
}
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
			   GLsizei height) {
	(void)target;
	(void)internalformat;
	(void)width;
	(void)height;
}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
			    GLuint texture, GLint level) {
	(void)target;
	(void)textarget;
	ShimFramebuffer* fb = framebuffer_by_id(g.cur_fbo);
	if (!fb) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (attachment != GL_COLOR_ATTACHMENT0 || level != 0) {
		shim_set_error(GL_INVALID_ENUM);
		return;
	}
	if (texture == 0) {
		fb->texid = 0;
		fb->rt_valid = 0;
		fb->width = fb->height = 0;
		g.draw.valid = 0;
		return;
	}
	ShimTexture* t = texture_by_id(texture);
	if (!t || t->width <= 0 || t->height <= 0) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (fb->rt_valid && fb->texid == texture)
		return;
	fb->rt_valid = 0;
	fb->texid = 0;
	if (!fbo_make_color_texture(fb, t)) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	g.draw.valid = 0;
#if defined(PS4_RENDER_CANARY)
	CANARY_MARK(CANARY_SLOT_FBO);
	if (CANARY_TAKE(CANARY_SLOT_FBO, 12))
		slog("canary: fbo tex id=%u fbo=%u %dx%d rt_valid=%d",
		     texture, fb->id, fb->width, fb->height, fb->rt_valid);
#endif
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
			       GLenum renderbuffertarget, GLuint renderbuffer) {
	(void)target;
	(void)attachment;
	(void)renderbuffertarget;
	(void)renderbuffer;
}
/* Submit the command buffer built so far, wait for the GPU, and start a new
 * one.  glReadPixels is called by Kodi's capture tap after all draws of the
 * frame and before present; the tiled render target is not readable by the
 * CPU until the GPU has finished it.  Submitting here keeps the already
 * rendered pixels in the target; present later submits the (empty) remainder
 * and flips the same buffer. */
static int readback_submit_and_wait(void) {
	if (g.cmd.cmdptr == g.cmd.beginptr)
		return 1;
	gnmDrawCmdEventWriteEop(&g.cmd, GNM_CACHE_FLUSH_TS, (uint64_t)g.label,
				GNM_DATA_SEL_SEND_DATA64, 1);
	*g.label = 0;
	void* dcbaddr = (void*)g.cmd.beginptr;
	uint32_t dcbsize = (uint32_t)(g.cmd.cmdptr - g.cmd.beginptr) * sizeof(uint32_t);
	if (sceGnmSubmitCommandBuffers(1, &dcbaddr, &dcbsize, NULL, 0) != 0) {
		slog("readpixels: SubmitCommandBuffers failed");
		*g.label = 1;
		begin_gpu_frame();
		g.draw.valid = 0;
		shim_set_error(GL_INVALID_OPERATION);
		return 0;
	}
	const uint64_t deadline = sceKernelGetProcessTime() + 5ull * 1000ull * 1000ull;
	while (*g.label != 1 && sceKernelGetProcessTime() < deadline) {
	}
	const int done = *g.label == 1;
	if (!done)
		slog("readpixels: GPU wait timeout (label=%llu)",
		     (unsigned long long)*g.label);
	sceGnmSubmitDone();
	begin_gpu_frame();
	g.draw.valid = 0;
	/* Make GPU writes visible to the CPU before the detile pass. */
	sceGnmFlushGarlic();
	return done;
}

/* Detile the current render target with the GPA tiler and copy the requested
 * region to GL pixel memory.  Kodi's GLES2 capture path requests RGBA/UNSIGNED
 * BYTE; BGRA and RGB are accepted for the other capture users. */
static int readpixels_rgba(GnmRenderTarget* rt, GLint x, GLint y, GLsizei width,
			   GLsizei height, GLenum format, void* pixels) {
	if (!rt || !pixels || width <= 0 || height <= 0)
		return 0;
	if (format != GL_RGBA && format != GL_RGB && format != GL_BGRA_EXT)
		return 0;

	const GLint rtw = (GLint)rt->size.width;
	const GLint rth = (GLint)rt->size.height;
	if (x < 0 || y < 0 || x > rtw || y > rth || width > rtw - x ||
	    height > rth - y) {
		slog("readpixels: invalid rect x=%d y=%d w=%d h=%d rt=%dx%d", x, y,
		     width, height, rtw, rth);
		return 0;
	}

	const uint32_t pitch = gnmRtGetPitch(rt);
	const uint32_t rows = gnmRtGetSliceSize(rt);
	const size_t outlen = (size_t)pitch * (size_t)rows * 4u;
	uint64_t inlen64 = 0;
	uint32_t inalign = 0;
	if (gnmRtCalcByteSize(&inlen64, &inalign, rt) != GNM_ERROR_OK ||
	    inlen64 == 0 || inlen64 > (uint64_t)SIZE_MAX) {
		slog("readpixels: cannot size render target");
		return 0;
	}
	const size_t inlen = (size_t)inlen64;
	if (outlen == 0 || outlen > 64u * 1024u * 1024u)
		return 0;

	uint8_t* linear = (uint8_t*)malloc(outlen);
	if (!linear) {
		slog("readpixels: no memory for %zu byte detile", outlen);
		return 0;
	}

	GpaTextureInfo texinfo = gnmRtBuildInfo(rt);
	GpaTilingParams src = {0};
	GpaTilingParams dst = {0};
	GpaError gerr = gpaTpInit(&src, &texinfo, 0, 0);
	if (gerr == GPA_ERR_OK) {
		GpaTextureInfo dstinfo = texinfo;
		dstinfo.tm = GNM_TM_DISPLAY_LINEAR_GENERAL;
		gerr = gpaTpInit(&dst, &dstinfo, 0, 0);
	}
	if (gerr != GPA_ERR_OK) {
		slog("readpixels: tiling params failed %s", gpaStrError(gerr));
		free(linear);
		return 0;
	}

	/* glReadPixels coordinates are bottom-left; the render target rows are
	 * top-down.  Detile the mirrored strip and copy it back bottom-up. */
	const uint32_t top = (uint32_t)(rth - (y + height));
	const GpaSurfaceRegion region = {
	    .left = (uint32_t)x,
	    .top = top,
	    .front = 0,
	    .right = (uint32_t)(x + width),
	    .bottom = top + (uint32_t)height,
	    .back = 1,
	};
	const uint64_t t0 = sceKernelGetProcessTime();
	gerr = gpaTileSurfaceRegion(linear, outlen, gnmRtGetBaseAddr(rt), inlen,
				    &src, &dst, &region);
	if (gerr != GPA_ERR_OK) {
		slog("readpixels: detile failed %s (%dx%d tile=%u pitch=%u rows=%u)",
		     gpaStrError(gerr), rtw, rth, rt->attrib.tilemode_index, pitch, rows);
		free(linear);
		return 0;
	}
	const uint64_t detile_us = sceKernelGetProcessTime() - t0;

	const int out_bpp = format == GL_RGB ? 3 : 4;
	const uint8_t* srcrow =
	    linear + (size_t)(top + (uint32_t)height - 1u) * pitch * 4u +
	    (size_t)x * 4u;
	uint8_t* dstrow = (uint8_t*)pixels;
	for (GLsizei row = 0; row < height; row++) {
		for (GLsizei col = 0; col < width; col++) {
			const uint8_t* s = srcrow + (size_t)col * 4u;
			uint8_t* d = dstrow + (size_t)col * (size_t)out_bpp;
			if (format == GL_RGBA) {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = s[3];
			} else if (format == GL_BGRA_EXT) {
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				d[3] = s[3];
			} else {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
			}
		}
		srcrow -= (size_t)pitch * 4u;
		dstrow += (size_t)width * (size_t)out_bpp;
	}
	free(linear);

	static int logged;
	if (!logged) {
		logged = 1;
		slog("readpixels: %dx%d fmt=0x%x rt=%dx%d tile=%u detile=%.3f ms",
		     width, height, format, rtw, rth, rt->attrib.tilemode_index,
		     detile_us / 1000.0);
	}
	return 1;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
		  GLenum type, void* pixels) {
	if (!g.ready)
		glShimInit(1920, 1080);
	if (!g.ready || !pixels)
		return;
	if (type != GL_UNSIGNED_BYTE) {
		/* Kodi's GLES2 capture path negotiates down to RGBA/UNSIGNED_BYTE;
		 * deep types are not exposed by this shim yet. */
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	GnmRenderTarget* rt = current_render_target();
	if (!rt) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (!readback_submit_and_wait()) {
		shim_set_error(GL_INVALID_OPERATION);
		return;
	}
	if (!readpixels_rgba(rt, x, y, width, height, format, pixels))
		shim_set_error(GL_INVALID_OPERATION);
}
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
		       GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
		       GLenum filter) {
	(void)srcX0;
	(void)srcY0;
	(void)srcX1;
	(void)srcY1;
	(void)dstX0;
	(void)dstY0;
	(void)dstX1;
	(void)dstY1;
	(void)mask;
	(void)filter;
}
void glReadBuffer(GLenum src) { (void)src; }
GLsync glFenceSync(GLenum condition, GLbitfield flags) {
	(void)condition;
	(void)flags;
	return (GLsync)(uintptr_t)1;
}
GLboolean glIsSync(GLsync sync) { return sync ? GL_TRUE : GL_FALSE; }
void glDeleteSync(GLsync sync) { (void)sync; }
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
	(void)sync;
	(void)flags;
	(void)timeout;
	return GL_CONDITION_SATISFIED;
}
void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
	(void)sync;
	(void)flags;
	(void)timeout;
}
