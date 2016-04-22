/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <getopt.h>

#include "bs_drm.h"

#define BUFFERS 2

struct framebuffer {
	struct gbm_bo *bo;
	int drm_prime_fd;
	uint32_t vgem_handle;
	uint32_t id;
};

struct context;
typedef uint32_t *(*mmap_t)(struct context *ctx, struct framebuffer *fb, size_t size);

struct context {
	int display_fd;
	int vgem_fd;
	uint32_t crtc_id;

	struct framebuffer fbs[BUFFERS];
	mmap_t mmap_fn;
};

static void disable_psr()
{
	const char psr_path[] = "/sys/module/i915/parameters/enable_psr";
	int psr_fd = open(psr_path, O_WRONLY);

	if (psr_fd < 0)
		return;

	if (write(psr_fd, "0", 1) == -1) {
		bs_debug_error("failed to disable psr");
	} else {
		printf("disabled psr");
	}

	close(psr_fd);
}

static void do_fixes()
{
	disable_psr();
}

#define STEP_SKIP 0
#define STEP_MMAP 1
#define STEP_FAULT 2
#define STEP_FLIP 3
#define STEP_DRAW 4

static void show_sequence(const int *sequence)
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

static void draw(struct context *ctx)
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
						bo_ptr = ctx->mmap_fn(ctx, fb, bo_size);
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

static int create_vgem(struct context *ctx)
{
	ctx->vgem_fd = bs_drm_open_vgem();
	if (ctx->vgem_fd < 0)
		return 1;
	return 0;
}

static int vgem_prime_fd_to_handle(struct context *ctx, struct framebuffer *fb)
{
	int ret = drmPrimeFDToHandle(ctx->vgem_fd, fb->drm_prime_fd, &fb->vgem_handle);
	if (ret)
		return 1;
	return 0;
}

static uint32_t *vgem_mmap_internal(struct context *ctx, struct framebuffer *fb, size_t size)
{
	return bs_dumb_mmap(ctx->vgem_fd, fb->vgem_handle, size);
}

static uint32_t *dma_buf_mmap_internal(struct context *ctx, struct framebuffer *fb, size_t size)
{
	return bs_dma_buf_mmap(fb->bo);
}

static const struct option longopts[] = {
	{ "help", no_argument, NULL, 'h' },
	{ "use_vgem", no_argument, NULL, 'v' },
	{ "use_dma_buf", no_argument, NULL, 'd' },
	{ 0, 0, 0, 0 },
};

static void print_help(const char *argv0)
{
	const char help_text[] =
	    "Usage: %s [OPTIONS]\n"
	    " -h, --help\n"
	    "           Print help.\n"
	    " -d, --use_dma_buf\n"
	    "           Use dma_buf mmap.\n"
	    " -v, --use_vgem\n"
	    "           Use vgem mmap.\n";
	printf(help_text, argv0);
}

int main(int argc, char **argv)
{
	struct context ctx = { 0 };

	bool is_vgem_test = false;
	bool is_help = false;
	ctx.mmap_fn = dma_buf_mmap_internal;
	int c;
	while ((c = getopt_long(argc, argv, "vdh", longopts, NULL)) != -1) {
		switch (c) {
			case 'v':
				ctx.mmap_fn = vgem_mmap_internal;
				is_vgem_test = true;
				break;
			case 'd':
				break;
			case 'h':
			default:
				is_help = true;
				break;
		}
	}

	if (is_help) {
		print_help(argv[0]);
		return 1;
	}

	if (is_vgem_test) {
		printf("started vgem mmap test.\n");
	} else {
		printf("started dma_buf mmap test.\n");
	}

	do_fixes();

	ctx.display_fd = bs_drm_open_main_display();
	if (ctx.display_fd < 0) {
		bs_debug_error("failed to open card for display");
		return 1;
	}

	if (is_vgem_test && create_vgem(&ctx)) {
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

		fb->drm_prime_fd = gbm_bo_get_fd(fb->bo);
		if (fb->drm_prime_fd < 0) {
			bs_debug_error("failed to turn handle into fd");
			return 1;
		}

		if (is_vgem_test && vgem_prime_fd_to_handle(&ctx, fb)) {
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
