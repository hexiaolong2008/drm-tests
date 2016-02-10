/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

struct bs_drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static int bs_drm_fb_create_fd_gbm(int fd, struct gbm_bo *bo, uint32_t *fb_id)
{
	assert(fd >= 0);
	assert(bo);
	assert(fb_id);

	uint32_t width = gbm_bo_get_width(bo);
	uint32_t height = gbm_bo_get_height(bo);
	uint32_t format = gbm_bo_get_format(bo);
	uint32_t handle = gbm_bo_get_handle(bo).u32;
	uint32_t stride = gbm_bo_get_stride(bo);
	uint32_t offset = 0;

	return drmModeAddFB2(fd, width, height, format, &handle, &stride, &offset, fb_id, 0);
}

uint32_t bs_drm_fb_create_gbm(struct gbm_bo *bo)
{
	assert(bo);

	struct gbm_device *gbm = gbm_bo_get_device(bo);
	assert(gbm);

	int fd = gbm_device_get_fd(gbm);
	if (fd < 0) {
		bs_debug_error("buffer object's device has invalud fd: %d", fd);
		return 0;
	}

	uint32_t fb_id;
	int ret = bs_drm_fb_create_fd_gbm(fd, bo, &fb_id);

	if (ret) {
		bs_debug_error("failed to create framebuffer from buffer object: %d", ret);
		return 0;
	}

	return fb_id;
}
