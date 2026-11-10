#include "displayctx.h"

#include <stdio.h>

#ifdef USE_OPENGNM
#include <compat/freegnm.h>
#endif

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <string.h>

/* Optional: present in libglshim, absent in the standalone engine. */
extern void glShimLog(const char* fmt, ...) __attribute__((weak));
static int g_early_handle = -1;
static void dlog(const char* what, int rc) {
	if (glShimLog)
		glShimLog("display: %s rc=%d", what, rc);
}

#ifndef ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9
#define ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9 0
#endif

#ifndef ORBIS_VIDEO_OUT_TILING_MODE_TILE
#define ORBIS_VIDEO_OUT_TILING_MODE_TILE 0
#endif

/* Not declared by the OpenOrbis headers.  The layout is the one used by
 * shadPS4/ChonkyStation4 and is filled by sceVideoOutGetVblankStatus. */
typedef struct {
	uint64_t count;
	uint64_t process_time;
	uint64_t tsc;
	uint64_t reserved;
	uint8_t flags;
	uint8_t pad1[7];
} DisplayVblankStatus;

/* liborbisrender finds the VideoOut handle that the game process already
 * owns: libSceVideoOut keeps it in a global, loaded with
 *   mov edi, [rip+disp] ; xor r15d,r15d
 * (8B 3D ?? ?? ?? ?? 45 31 FF).  If valid, reuse it instead of opening a
 * new port (sceVideoOutOpen blocks in Kodi's process on this firmware). */
static int find_system_videoout_handle(void) {
	OrbisKernelModule mods[256];
	size_t avail = 0;
	int mlrc = sceKernelGetModuleList(mods, 256, &avail);
	glShimLog("display: modulelist rc=%d avail=%zu", mlrc, avail);
	if (mlrc != 0)
		return -1;
	int foundmod = 0;
	for (size_t i = 0; i < avail && i < 256; i++) {
		OrbisKernelModuleInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(mods[i], &info) != 0)
			continue;
		glShimLog("display: module[%zu] %s", i, info.name);
		if (!strstr(info.name, "libSceVideoOut"))
			continue;
		foundmod = 1;
		if (info.segmentCount == 0)
			continue;
		const uint8_t* base = (const uint8_t*)info.segmentInfo[0].address;
		const uint8_t* end = base + info.segmentInfo[0].size;
		glShimLog("display: found %s base=%p size=%u", info.name, (void*)base,
			  info.segmentInfo[0].size);
		for (const uint8_t* p = base; p + 9 <= end; p++) {
			if (p[0] != 0x8B || p[1] != 0x3D || p[6] != 0x45 ||
			    p[7] != 0x31 || p[8] != 0xFF)
				continue;
			int32_t disp;
			memcpy(&disp, p + 2, sizeof(disp));
			const int32_t* target = (const int32_t*)(p + 6 + disp);
			int handle;
			memcpy(&handle, target, sizeof(handle));
			glShimLog("display: videoout global at %p value=%d", (void*)target,
				  handle);
			OrbisVideoOutResolutionStatus st;
			memset(&st, 0, sizeof(st));
			int rc = sceVideoOutGetResolutionStatus(handle, &st);
			glShimLog("display: handle %d resolution rc=%d %ux%u refresh=%llu flags=%04x",
				  handle, rc, st.width, st.height,
				  (unsigned long long)st.refreshRate, st.flags);
			if (rc == 0 && st.width > 0 && st.height > 0)
				return handle;
		}
	}
	if (!foundmod)
		glShimLog("display: libSceVideoOut not found in module list");
	return -1;
}

void displayctx_early_open(void) {
	if (g_early_handle >= 0)
		return;
	/* Kodi's process reaches this point with libSceVideoOut *not loaded*
	 * (module list: eboot, libkernel_sys, libSceLibcInternal,
	 * libSceSysmodule, libkernel).  The lazy module load triggered by the
	 * first import call blocks; load the modules explicitly first, as
	 * liborbisrender does. */
	dlog("loadmodule-gnm-begin", 0);
	int gnmmod = sceKernelLoadStartModule("libSceGnmDriver.sprx", 0, NULL, 0,
					      NULL, NULL);
	dlog("loadmodule-gnm-end", gnmmod);
	dlog("loadmodule-videoout-begin", 0);
	int vomod = sceKernelLoadStartModule("libSceVideoOut.sprx", 0, NULL, 0,
					     NULL, NULL);
	dlog("loadmodule-videoout-end", vomod);
	int sysh = find_system_videoout_handle();
	if (sysh >= 0) {
		dlog("reusing system handle", sysh);
		g_early_handle = sysh;
		return;
	}
	dlog("early-open-begin", 0);
	g_early_handle = sceVideoOutOpen(0, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
	dlog("early-open-end", g_early_handle);
}

bool displayctx_init(DisplayContext* ctx) {
	int res;
	if (g_early_handle >= 0) {
		res = g_early_handle;
		g_early_handle = -1;
		dlog("open-early-reuse", res);
	} else {
		dlog("open-begin", 0);
		res = sceVideoOutOpen(0, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
		dlog("open-end", res);
	}
	if (res < 0) {
		printf("display: VideoOutOpen failed with 0x%x\n", res);
		return false;
	}

	ctx->videohandle = res;
	ctx->flip_seq = 0;
	ctx->pending_flip_num = 0;
	ctx->last_vblank_count = 0;
	ctx->vblank_valid = 0;
	ctx->last_present_us = sceKernelGetProcessTime();
	{
		DisplayVblankStatus vst = {0};
		sceVideoOutGetVblankStatus(ctx->videohandle, &vst);
		ctx->last_vblank_count = vst.count;
		ctx->vblank_valid = 1;
		if (glShimLog)
			glShimLog("display: vblank initial count=%llu proc=%llu tsc=%llu",
				  (unsigned long long)vst.count,
				  (unsigned long long)vst.process_time,
				  (unsigned long long)vst.tsc);
	}

	OrbisVideoOutResolutionStatus resinfo = {0};
	res = sceVideoOutGetResolutionStatus(ctx->videohandle, &resinfo);
	dlog("resolution", res);
	if (res != 0) {
		printf("display: GetResolution failed with 0x%x\n", res);
		return false;
	}

	ctx->screenw = resinfo.width;
	ctx->screenh = resinfo.height;
	if (glShimLog)
		glShimLog("display: mode %ux%u pane %ux%u refresh=%llu screen=%.1f flags=%04x",
			  resinfo.width, resinfo.height, resinfo.paneWidth, resinfo.paneHeight,
			  (unsigned long long)resinfo.refreshRate, resinfo.screenSize,
			  resinfo.flags);

	res = sceKernelCreateEqueue(&ctx->flipqueue, "display flip queue");
	dlog("equeue", res);
	if (res != 0) {
		printf("display: CreateEqueue failed with 0x%x\n", res);
		return false;
	}

	sceVideoOutSetFlipRate(ctx->videohandle, 0);
	/* Pacing uses sceVideoOutWaitVblank() directly.  Do not register a
	 * vblank equeue: events accumulate while the CPU is busy and the wait
	 * then returns immediately for every stale event, which is exactly the
	 * 300-500 fps transition problem we are fixing. */

	return true;
}

bool displayctx_setrts(
	DisplayContext* ctx, GnmRenderTarget* rts, unsigned numrts
) {
	if (!ctx || !rts || !numrts) {
		return false;
	}

	for (unsigned i = 0; i < numrts; i += 1) {
		GnmRenderTarget* rt = &rts[i];
		OrbisVideoOutBufferAttribute attr = {0};
		sceVideoOutSetBufferAttribute(
			&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
			ORBIS_VIDEO_OUT_TILING_MODE_TILE, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
			rt->size.width, rt->size.height, gnmRtGetPitch(rt)
		);

		void* rtaddr = gnmRtGetBaseAddr(rt);
		int res =
			sceVideoOutRegisterBuffers(ctx->videohandle, i, &rtaddr, 1, &attr);
		if (res < 0) {
			printf("display: RegisterBuffers failed with 0x%x\n", res);
			return false;
		}
	}

	return true;
}

bool displayctx_submit_flip(DisplayContext* ctx, unsigned bufidx) {
	OrbisVideoOutFlipStatus st = {0};
	if (sceVideoOutGetFlipStatus(ctx->videohandle, &st) == 0)
		ctx->pending_flip_num = st.num + 1;
	else
		ctx->pending_flip_num = 0;

	ctx->flip_seq++;
	int res = sceVideoOutSubmitFlip(
		ctx->videohandle, bufidx, ORBIS_VIDEO_OUT_FLIP_VSYNC,
		(int64_t)ctx->flip_seq
	);
	if (res != 0) {
		printf("display: SubmitFlip failed with 0x%x\n", res);
		return false;
	}
	return true;
}

bool displayctx_wait_flip(DisplayContext* ctx) {
	if (!ctx->videohandle)
		return false;

	/* The hardware vblank counter advances once per real display refresh
	 * (16.686 ms at 59.94 Hz on this firmware).  Waiting for the *next*
	 * count, not merely for a change, keeps one flip per vblank even when
	 * the caller was busy for several vblanks.  A stale counter falls back
	 * to the monotonic 59.94 Hz limiter below. */
	const uint64_t frame_us = 16683;
	DisplayVblankStatus vst = {0};
	sceVideoOutGetVblankStatus(ctx->videohandle, &vst);
	bool have_vblank = true;
	if (have_vblank && ctx->vblank_valid) {
		const uint64_t target = ctx->last_vblank_count + 1;
		const uint64_t deadline = sceKernelGetProcessTime() + 50000ull;
		while ((int64_t)(vst.count - target) < 0) {
			if (sceKernelGetProcessTime() > deadline) {
				static int timeout_logs;
				if (glShimLog && timeout_logs < 5) {
					glShimLog("display: vblank wait timeout count=%llu target=%llu",
						  (unsigned long long)vst.count,
						  (unsigned long long)target);
					timeout_logs++;
				}
				have_vblank = false;
				break;
			}
			sceKernelUsleep(500);
			sceVideoOutGetVblankStatus(ctx->videohandle, &vst);
		}
		if (have_vblank) {
			ctx->last_vblank_count = vst.count;
			ctx->last_present_us = sceKernelGetProcessTime();
			return true;
		}
	}
	if (have_vblank) {
		ctx->last_vblank_count = vst.count;
		ctx->vblank_valid = 1;
	}

	const uint64_t now = sceKernelGetProcessTime();
	if (ctx->last_present_us == 0)
		ctx->last_present_us = now;
	const uint64_t target = ctx->last_present_us + frame_us;
	if (now < target) {
		uint64_t remaining = target - now;
		while (remaining > 0) {
			if (remaining > 1000)
				sceKernelUsleep((uint32_t)(remaining - 500));
			else
				sceKernelUsleep((uint32_t)remaining);
			const uint64_t after = sceKernelGetProcessTime();
			if (after >= target)
				break;
			remaining = target - after;
		}
	}
	ctx->last_present_us = sceKernelGetProcessTime();
	return true;
}

bool displayctx_flip(DisplayContext* ctx, unsigned bufidx) {
	return displayctx_submit_flip(ctx, bufidx) && displayctx_wait_flip(ctx);
}

void displayctx_destroy(DisplayContext* ctx) {
	if (ctx->videohandle) {
		sceVideoOutClose(ctx->videohandle);
		ctx->videohandle = 0;
	}
	if (ctx->flipqueue) {
		sceKernelDeleteEqueue(ctx->flipqueue);
		ctx->flipqueue = 0;
	}
}
