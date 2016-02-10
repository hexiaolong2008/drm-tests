/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

int main()
{
	int fd = bs_drm_open_main_display();
	if (fd < 0) {
		bs_debug_error("failed to open card for display");
		return 1;
	}

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm");
		return 1;
	}

	struct bs_drm_pipe pipe = {0};
	if (!bs_drm_pipe_make(fd, &pipe)) {
		bs_debug_error("failed to make pipe");
		return 1;
	}

	drmModeConnector *connector = drmModeGetConnector(fd, pipe.connector_id);
	drmModeModeInfo *mode = &connector->modes[0];

	struct gbm_bo *bos[2];
	uint32_t ids[2];
	for (size_t fb_index = 0; fb_index < 2; fb_index++) {
		bos[fb_index] =
		    gbm_bo_create(gbm, mode->hdisplay, mode->vdisplay, GBM_FORMAT_XRGB8888,
				  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (bos[fb_index] == NULL) {
			bs_debug_error("failed to allocate frame buffer");
			return 1;
		}
		ids[fb_index] = bs_drm_fb_create_gbm(bos[fb_index]);
		if (ids[fb_index] == 0) {
			bs_debug_error("failed to create framebuffer id");
			return 1;
		}
	}

	for (size_t frame_index = 0; frame_index < 10000; frame_index++) {
		size_t fb_index = frame_index % 2;
		struct gbm_bo *bo = bos[fb_index];
		size_t bo_size = gbm_bo_get_stride(bo) * mode->vdisplay;
		char *ptr = bs_dumb_mmap_gbm(bo);
		for (size_t i = 0; i < bo_size / 4; i++) {
			ptr[i * 4 + 0] = (i + frame_index * 50) % 256;
			ptr[i * 4 + 1] = (i + frame_index * 50 + 85) % 256;
			ptr[i * 4 + 2] = (i + frame_index * 50 + 170) % 256;
			ptr[i * 4 + 3] = 0;
		}
		bs_dumb_unmmap_gbm(bo, ptr);

		int ret = drmModeSetCrtc(fd, pipe.crtc_id, ids[fb_index], 0 /* x */, 0 /* y */,
					 &pipe.connector_id, 1 /* connector count */, mode);
		if (ret) {
			bs_debug_error("failed to set crtc: %d", ret);
			return 1;
		}
		usleep(16667);
	}

	for (size_t fb_index = 0; fb_index < 2; fb_index++) {
		gbm_bo_destroy(bos[fb_index]);
		drmModeRmFB(fd, ids[fb_index]);
	}

	return 0;
}
