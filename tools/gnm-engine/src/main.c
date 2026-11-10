/*
 * gnm-engine — minimal GPU render engine probe for Kodi PS4.
 *
 * Renders a fullscreen textured quad with the Kodi splash image using
 * sceGnm + sceVideoOut directly (no Piglet, no OpenGL).  This is the
 * seed for the custom render engine: once this works on hardware, the
 * same pipeline (textures, VBOs, shaders, descriptor sets, flips) is
 * what the GLES-subset shim will map Kodi's GUI calls onto.
 *
 * Based on the freegnm-examples cube sample (MIT).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <gnm/drawcommandbuffer.h>
#include <gnm/gpuaddr/gpuaddr.h>
#include <gnm/platform.h>
#include <compat/freegnm.h>

#include "u/utility.h"

#include "displayctx.h"
#include "memalloc.h"
#include "misc.h"

static void glog(const char* fmt, ...) {
	FILE* f = fopen("/data/gnm-engine.log", "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

typedef struct {
	float pos[2];
	float uv[2];
} Vertex;

// Centered 625x625 quad (aspect-correct on 1920x1080), UVs cropped to
// the real image area inside the padded 640x632 texture. V is flipped so
// the PNG (top-left origin) samples upright with the GL-style viewport.
#define IMG_W 625.0f
#define IMG_H 625.0f
#define TEX_W 640.0f
#define TEX_H 632.0f
#define SCR_W 1920.0f
#define SCR_H 1080.0f
static const float kHalfX = IMG_W / SCR_W;
static const float kHalfY = IMG_H / SCR_H;
static const float kU = IMG_W / TEX_W;
static const float kV = IMG_H / TEX_H;

static const Vertex s_vertices[] = {
    {{-kHalfX, -kHalfY}, {0.0f, kV}},
    {{kHalfX, -kHalfY}, {kU, kV}},
    {{kHalfX, kHalfY}, {kU, 0.0f}},
    {{-kHalfX, -kHalfY}, {0.0f, kV}},
    {{kHalfX, kHalfY}, {kU, 0.0f}},
    {{-kHalfX, kHalfY}, {0.0f, 0.0f}},
};

typedef struct {
	GnmTexture texture;
	GnmSampler sampler;
} PsDescSet;
_Static_assert(sizeof(PsDescSet) == 0x30, "");

int main(void) {
	MemoryAllocator garlicmem = memalloc_init(
		64 * 1024 * 1024,
		ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW |
		    ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
		ORBIS_KERNEL_WC_GARLIC);

	glog("engine: start");
	if (!initclearutility(&garlicmem)) {
		glog("engine: clear utility failed");
		fatal("Failed to init clear utility");
	}
	glog("engine: clear utility ok");

	DisplayContext display = {0};
	if (!displayctx_init(&display)) {
		glog("engine: displayctx_init failed");
		fatal("Failed to init video");
	}
	glog("engine: display %ux%u", display.screenw, display.screenh);
	printf("gnm-engine: display %ux%u\n", display.screenw, display.screenh);

	GnmRenderTarget fbtarget = {0};
	if (!initcolortarget(&fbtarget, &garlicmem, display.screenw, display.screenh,
			     GNM_FMT_R8G8B8A8_SRGB, gnmGpuMode())) {
		fatal("Failed to init color render target");
	}
	if (!displayctx_setrts(&display, &fbtarget, 1)) {
		fatal("Failed to register render target");
	}

	const size_t cmdmemsize = GNM_INDIRECT_BUFFER_MAX_BYTESIZE;
	void* cmdmem = memalloc_alloc(&garlicmem, cmdmemsize, GNM_ALIGNMENT_BUFFER_BYTES);
	if (!cmdmem) {
		fatal("Failed to allocate command buffer");
	}
	GnmCommandBuffer cmdbuf = gnmCmdInit(cmdmem, cmdmemsize, NULL, NULL);

	GnmVsShader* vshader = NULL;
	GnmPsShader* pshader = NULL;
	if (!loadvshader(&vshader, &garlicmem, "/app0/assets/misc/quad.vert.sb")) {
		fatal("Failed to load vertex shader");
	}
	if (!loadpshader(&pshader, &garlicmem, "/app0/assets/misc/quad.frag.sb")) {
		fatal("Failed to load fragment shader");
	}

	GnmTexture texture = {0};
	if (!loadlineartexturefile(&garlicmem, &texture,
				   "/app0/assets/images/splash.png")) {
		glog("engine: texture load failed");
		fatal("Failed to load splash texture");
	}
	glog("engine: texture ok (%ux%u)", texture.width, texture.height);

	const GnmSampler sampler = {
	    .xyminfilter = GNM_FILTER_BILINEAR,
	    .xymagfilter = GNM_FILTER_BILINEAR,
	    .mipfilter = GNM_MIPFILTER_NONE,
	    .zfilter = GNM_ZFILTER_POINT,
	    .minlod = 0,
	    .maxlod = 0xfff,
	};

	PsDescSet* psdescset =
	    memalloc_alloc(&garlicmem, sizeof(PsDescSet), GNM_ALIGNMENT_BUFFER_BYTES);
	if (!psdescset) {
		fatal("Failed to allocate PS descriptor set");
	}
	*psdescset = (PsDescSet){.texture = texture, .sampler = sampler};

	Vertex* vertmem =
	    memalloc_alloc(&garlicmem, sizeof(s_vertices), GNM_ALIGNMENT_BUFFER_BYTES);
	if (!vertmem) {
		fatal("Failed to allocate vertex data");
	}
	memcpy(vertmem, s_vertices, sizeof(s_vertices));

	GnmBuffer* vertbuffers =
	    memalloc_alloc(&garlicmem, sizeof(GnmBuffer) * 2, GNM_ALIGNMENT_BUFFER_BYTES);
	if (!vertbuffers) {
		fatal("Failed to allocate vertex buffers");
	}
	// buffer 0: position (offset 0), buffer 1: uv (offset 8)
	vertbuffers[0] = gnmCreateVertexBuffer(vertmem, GNM_FMT_R32G32_FLOAT,
					       sizeof(Vertex), 6);
	vertbuffers[1] = gnmCreateVertexBuffer((char*)vertmem + 8, GNM_FMT_R32G32_FLOAT,
					       sizeof(Vertex), 6);

	const GnmFetchShaderCreateInfo fetchci = {
	    .regs = &vshader->registers,
	    .vtxinputs = gnmVsShaderInputSemanticTable(vshader),
	    .numvtxinputs = vshader->numinputsemantics,
	    .inputusages = gnmVsShaderInputUsageSlotTable(vshader),
	    .numinputusages = vshader->common.numinputusageslots,
	};
	uint32_t fetchsize = 0;
	GnmError err = gnmFetchShaderCalcSize(&fetchsize, &fetchci);
	if (err != GNM_ERROR_OK) {
		fatalf("Fetch shader size failed: %s", gnmStrError(err));
	}
	void* fetchshmem =
	    memalloc_alloc(&garlicmem, fetchsize, GNM_ALIGNMENT_FETCHSHADER_BYTES);
	if (!fetchshmem) {
		fatal("Failed to allocate fetch shader");
	}
	GnmFetchShaderResults fetchres = {0};
	err = gnmCreateFetchShader(fetchshmem, fetchsize, &fetchci, &fetchres);
	if (err != GNM_ERROR_OK) {
		fatalf("Fetch shader create failed: %s", gnmStrError(err));
	}
	gnmVsRegsSetFetchShaderModifier(&vshader->registers, &fetchres);

	uint64_t* label =
	    memalloc_alloc(&garlicmem, sizeof(uint64_t), sizeof(uint64_t));
	if (!label) {
		fatal("Failed to allocate sync label");
	}

	const GnmPrimitiveSetup primsetup = {
	    .cullmode = GNM_CULL_NONE,
	    .frontface = GNM_FACE_CCW,
	    .frontmode = GNM_FILL_SOLID,
	    .backmode = GNM_FILL_SOLID,
	    .frontoffsetmode = false,
	    .backoffsetmode = false,
	    .vertexwindowoffsetenable = false,
	    .provokemode = GNM_PROVOKINGVTX_FIRST,
	    .perspectivecorrectiondisable = false,
	};

	unsigned long long frame = 0;
	while (1) {
		gnmCmdReset(&cmdbuf);
		*label = 9;

		const float clearcolor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		clearcolortarget(&cmdbuf, &fbtarget, clearcolor);

		gnmDrawCmdWaitGraphicsWrite(&cmdbuf, GNM_ACQUIRE_TARGET_CB0 | GNM_ACQUIRE_TARGET_DB);
		gnmDrawCmdSetPrimitiveSetup(&cmdbuf, &primsetup);
		gnmDrawCmdSetRenderTarget(&cmdbuf, 0, &fbtarget);
		gnmDrawCmdSetRenderTargetMask(&cmdbuf, 0xf);
		setupviewport(&cmdbuf, 0, 0, display.screenw, display.screenh, 0.5f, 0.5f);

		gnmDrawCmdSetVsShader(&cmdbuf, &vshader->registers, 0);
		gnmDrawCmdSetPsShader(&cmdbuf, &pshader->registers);
		gnmDrawCmdSetPsInputUsage(&cmdbuf,
					  gnmVsShaderExportSemanticTable(vshader),
					  vshader->numexportsemantics,
					  gnmPsShaderInputSemanticTable(pshader),
					  pshader->numinputsemantics);

		gnmDrawCmdSetPointerUserData(&cmdbuf, GNM_STAGE_VS, 0, fetchshmem);
		gnmDrawCmdSetPointerUserData(&cmdbuf, GNM_STAGE_VS, 2, vertbuffers);
		gnmDrawCmdSetPointerUserData(&cmdbuf, GNM_STAGE_PS, 0, psdescset);

		if (frame == 0)
			glog("engine: first draw issued");
		gnmDrawCmdSetPrimitiveType(&cmdbuf, GNM_PT_TRILIST);
		gnmDrawCmdDrawIndexAuto(&cmdbuf, 6);

		gnmDrawCmdEventWriteEop(&cmdbuf, GNM_CACHE_FLUSH_TS, (uint64_t)label,
					GNM_DATA_SEL_SEND_DATA64, 1);

		void* dcbaddr = (void*)cmdbuf.beginptr;
		uint32_t dcbsize = (cmdbuf.cmdptr - cmdbuf.beginptr) * sizeof(uint32_t);
		if (sceGnmSubmitCommandBuffers(1, &dcbaddr, &dcbsize, NULL, 0) != 0) {
			puts("submit failed");
			break;
		}
		while (*label != 1) {
			continue;
		}
		if (!displayctx_flip(&display, 0)) {
			glog("engine: flip failed at frame %llu", frame);
			break;
		}
		if (frame == 0)
			glog("engine: first flip ok");
		sceGnmSubmitDone();

		if ((frame % 120ull) == 0ull) {
			glog("engine: frame %llu", frame);
		}
		frame += 1;
	}

	displayctx_destroy(&display);
	memalloc_destroy(&garlicmem);
	return 0;
}
