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
	size_t plane_count = gbm_bo_get_num_planes(bo);

	if (plane_count == 0 || plane_count > 4) {
		bs_debug_error("buffer object has invalid number of planes: %zu", plane_count);
		return -EINVAL;
	}

	uint32_t handles[4] = { 0 };
	uint32_t strides[4] = { 0 };
	uint32_t offsets[4] = { 0 };

	for (size_t plane_index = 0; plane_index < plane_count; plane_index++) {
		handles[plane_index] = gbm_bo_get_plane_handle(bo, plane_index).u32;
		if (handles[plane_index] == 0) {
			bs_debug_error("buffer object has missing plane handle (index %zu)",
				       plane_index);
			return -EINVAL;
		}
		strides[plane_index] = gbm_bo_get_plane_stride(bo, plane_index);
		if (strides[plane_index] == 0) {
			bs_debug_error("buffer object has plane stride 0 (index %zu)", plane_index);
			return -EINVAL;
		}
		offsets[plane_index] = gbm_bo_get_plane_offset(bo, plane_index);
	}

	return drmModeAddFB2(fd, width, height, format, handles, strides, offsets, fb_id, 0);
}

uint32_t bs_drm_fb_create_gbm(struct gbm_bo *bo)
{
	assert(bo);

	struct gbm_device *gbm = gbm_bo_get_device(bo);
	assert(gbm);

	int fd = gbm_device_get_fd(gbm);
	if (fd < 0) {
		bs_debug_error("buffer object's device has invalid fd: %d", fd);
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
