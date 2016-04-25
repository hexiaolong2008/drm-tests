/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

#define BUFFERS 2

struct framebuffer {
	struct gbm_bo *bo;
	uint32_t vgem_handle;
	uint32_t id;
};

struct context {
	int display_fd;
	int vgem_fd;
	uint32_t crtc_id;

	struct framebuffer fbs[BUFFERS];
};

void disable_psr()
{
	const char psr_path[] = "/sys/module/i915/parameters/enable_psr";
	int psr_fd = open(psr_path, O_WRONLY);

	if (psr_fd < 0)
		return;

	if (write(psr_fd, "0", 1) == -1) {
		bs_debug_error("failed to disable psr");
	}
	else {
		printf("disabled psr");
	}

	close(psr_fd);
}

void do_fixes()
{
	disable_psr();
}

#define STEP_SKIP 0
#define STEP_MMAP 1
#define STEP_FAULT 2
#define STEP_FLIP 3
#define STEP_DRAW 4

void show_sequence(const int *sequence)
{
	int sequence_subindex;
	printf("starting sequence: ");
	for (sequence_subindex = 0; sequence_subindex < 4; sequence_subindex++) {
		switch (sequence[sequence_subindex]) {
			case STEP_SKIP:
				break;
			case STEP_MMAP:
				printf("mmap ");
				break;
			case STEP_FAULT:
				printf("fault ");
				break;
			case STEP_FLIP:
				printf("flip ");
				break;
			case STEP_DRAW:
				printf("draw ");
				break;
			default:
				bs_debug_error("<unknown step %d> (aborting!)",
					       sequence[sequence_subindex]);
				abort();
				break;
		}
	}
	printf("\n");
}

void draw(struct context *ctx)
{
	// Run the drawing routine with the key driver events in different
	// sequences.
	const int sequences[4][4] = {
		{ STEP_MMAP, STEP_FAULT, STEP_FLIP, STEP_DRAW },
		{ STEP_MMAP, STEP_FLIP, STEP_DRAW, STEP_SKIP },
		{ STEP_MMAP, STEP_DRAW, STEP_FLIP, STEP_SKIP },
		{ STEP_FLIP, STEP_MMAP, STEP_DRAW, STEP_SKIP },
	};

	int sequence_index = 0;
	int sequence_subindex = 0;

	int fb_idx = 1;

	for (sequence_index = 0; sequence_index < 4; sequence_index++) {
		show_sequence(sequences[sequence_index]);
		for (int frame_index = 0; frame_index < 0x100; frame_index++) {
			struct framebuffer *fb = &ctx->fbs[fb_idx];
			size_t bo_stride = gbm_bo_get_stride(fb->bo);
			size_t bo_size = bo_stride * gbm_bo_get_height(fb->bo);
			uint32_t *bo_ptr;
			volatile uint32_t *ptr;

			for (sequence_subindex = 0; sequence_subindex < 4; sequence_subindex++) {
				switch (sequences[sequence_index][sequence_subindex]) {
					case STEP_MMAP:
						bo_ptr = bs_dumb_mmap(ctx->vgem_fd, fb->vgem_handle,
								      bo_size);
						ptr = bo_ptr;
						break;

					case STEP_FAULT:
						*ptr = 1234567;
						break;

					case STEP_FLIP:
						drmModePageFlip(ctx->display_fd, ctx->crtc_id,
								ctx->fbs[fb_idx].id, 0, NULL);
						break;

					case STEP_DRAW:
						for (ptr = bo_ptr;
						     ptr < bo_ptr + (bo_size / sizeof(*bo_ptr));
						     ptr++) {
							int y = ((void *)ptr - (void *)bo_ptr) /
								bo_stride;
							int x = ((void *)ptr - (void *)bo_ptr -
								 bo_stride * y) /
								sizeof(*ptr);
							x -= 100;
							y -= 100;
							*ptr = 0xff000000;
							if (x * x + y * y <
							    frame_index * frame_index)
								*ptr |= (frame_index % 0x100) << 8;
							else
								*ptr |= 0xff |
									(sequence_index * 64 << 16);
						}
						break;

					case STEP_SKIP:
					default:
						break;
				}
			}

			munmap(bo_ptr, bo_size);

			usleep(1e6 / 120); /* 120 Hz */

			fb_idx = fb_idx ^ 1;
		}
	}
}

int main(int argc, char **argv)
{
	struct context ctx = { 0 };

	do_fixes();

	ctx.display_fd = bs_drm_open_main_display();
	if (ctx.display_fd < 0) {
		bs_debug_error("failed to open card for display");
		return 1;
	}

	ctx.vgem_fd = bs_drm_open_vgem();
	if (ctx.vgem_fd < 0) {
		bs_debug_error("failed to open vgem card");
		return 1;
	}

	struct gbm_device *gbm = gbm_create_device(ctx.display_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm device");
		return 1;
	}

	struct bs_drm_pipe pipe = { 0 };
	if (!bs_drm_pipe_make(ctx.display_fd, &pipe)) {
		bs_debug_error("failed to make pipe");
		return 1;
	}

	drmModeConnector *connector = drmModeGetConnector(ctx.display_fd, pipe.connector_id);
	drmModeModeInfo *mode = &connector->modes[0];
	ctx.crtc_id = pipe.crtc_id;

	printf("display size: %ux%u\n", mode->hdisplay, mode->vdisplay);

	for (size_t fb_index = 0; fb_index < BUFFERS; ++fb_index) {
		struct framebuffer *fb = &ctx.fbs[fb_index];
		fb->bo = gbm_bo_create(gbm, mode->hdisplay, mode->vdisplay, GBM_FORMAT_XRGB8888,
				       GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);

		if (!fb->bo) {
			bs_debug_error("failed to create buffer object");
			return 1;
		}

		fb->id = bs_drm_fb_create_gbm(fb->bo);
		if (fb->id == 0) {
			bs_debug_error("failed to create fb");
			return 1;
		}

		int drm_prime_fd = gbm_bo_get_fd(fb->bo);

		if (drm_prime_fd < 0) {
			bs_debug_error("failed to turn handle into fd");
			return 1;
		}

		int ret = drmPrimeFDToHandle(ctx.vgem_fd, drm_prime_fd, &fb->vgem_handle);
		if (ret) {
			bs_debug_error("failed to import handle");
			return 1;
		}
	}

	if (drmModeSetCrtc(ctx.display_fd, pipe.crtc_id, ctx.fbs[0].id, 0, 0, &pipe.connector_id, 1,
			   mode)) {
		bs_debug_error("failed to set CRTC");
		return 1;
	}

	draw(&ctx);

	return 0;
}
