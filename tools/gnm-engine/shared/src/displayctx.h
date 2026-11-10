#ifndef _DISPLAYCTX_H_
#define _DISPLAYCTX_H_

#include <orbis/libkernel.h>

#include <gnm/rendertarget.h>

typedef struct {
	int videohandle;
	OrbisKernelEqueue flipqueue;
	uint32_t screenw;
	uint32_t screenh;
	/* Each submitted flip gets a monotonically increasing arg.  Waiting for
	 * that specific flip (instead of "any" equeue event) prevents the close
	 * animation loop from consuming stale events and running at 300+ fps. */
	uint64_t flip_seq;
	uint64_t pending_flip_num;
	uint64_t last_vblank_count;
	int vblank_valid;
	uint64_t last_present_us;
} DisplayContext;

bool displayctx_init(DisplayContext* ctx);
void displayctx_early_open(void);
bool displayctx_setrts(
	DisplayContext* ctx, GnmRenderTarget* rts, unsigned numrts
);
bool displayctx_submit_flip(DisplayContext* ctx, unsigned bufidx);
bool displayctx_wait_flip(DisplayContext* ctx);
bool displayctx_flip(DisplayContext* ctx, unsigned bufidx);
void displayctx_destroy(DisplayContext* ctx);

#endif	// _DISPLAYCTX_H_
