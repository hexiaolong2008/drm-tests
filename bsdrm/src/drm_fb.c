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

struct bs_drm_fb *bs_drm_fb_new(struct gbm_device *gbm, uint32_t width, uint32_t height,
				uint32_t format, uint32_t flags)
{
	assert(gbm);

	int fd = gbm_device_get_fd(gbm);
	if (fd < 0)
		return NULL;

	struct bs_drm_fb *self = calloc(1, sizeof(struct bs_drm_fb));
	assert(self);

	self->bo = gbm_bo_create(gbm, width, height, format, flags);
	if (self->bo == NULL)
		goto out_destroy;

	uint32_t handle = gbm_bo_get_handle(self->bo).u32;
	uint32_t stride = gbm_bo_get_stride(self->bo);
	uint32_t offset = 0;

	int ret =
	    drmModeAddFB2(fd, width, height, format, &handle, &stride, &offset, &self->fb_id, 0);

	if (ret) {
		gbm_bo_destroy(self->bo);
		goto out_destroy;
	}

	return self;

out_destroy:

	free(self);

	return NULL;
}

void bs_drm_fb_destroy(struct bs_drm_fb **fb)
{
	assert(fb);

	struct bs_drm_fb *self = *fb;
	assert(self);

	struct gbm_device *gbm = gbm_bo_get_device(self->bo);
	assert(gbm);

	int fd = gbm_device_get_fd(gbm);
	assert(fd >= 0);

	gbm_bo_destroy(self->bo);
	drmModeRmFB(fd, self->fb_id);

	free(self);
	*fb = NULL;
}

struct gbm_bo *bs_drm_fb_bo(struct bs_drm_fb *self)
{
	assert(self);
	assert(self->bo);
	return self->bo;
}

uint32_t bs_drm_fb_id(struct bs_drm_fb *self)
{
	assert(self);
	return self->fb_id;
}
