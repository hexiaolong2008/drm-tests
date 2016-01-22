/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

void *bs_dumb_mmap(int fd, uint32_t handle, size_t size)
{
	assert(fd >= 0);

	struct drm_mode_map_dumb mmap_arg = {0};

	mmap_arg.handle = handle;

	int ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mmap_arg);
	if (ret != 0)
		return NULL;

	if (mmap_arg.offset == 0)
		return NULL;

	void *ptr = mmap(NULL, size, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, mmap_arg.offset);

	if (ptr == MAP_FAILED)
		return NULL;

	return ptr;
}

void *bs_dumb_mmap_gbm(struct gbm_bo *bo)
{
	assert(bo);

	uint32_t handle = gbm_bo_get_handle(bo).u32;
	size_t size = gbm_bo_get_stride(bo) * gbm_bo_get_height(bo);

	struct gbm_device *gbm = gbm_bo_get_device(bo);
	assert(gbm);

	int fd = gbm_device_get_fd(gbm);

	return bs_dumb_mmap(fd, handle, size);
}

int bs_dumb_unmmap_gbm(struct gbm_bo *bo, void *addr)
{
	assert(bo);
	assert(addr != NULL);
	assert(addr != MAP_FAILED);

	size_t size = gbm_bo_get_stride(bo) * gbm_bo_get_height(bo);
	int ret = munmap(addr, size);

	if (ret != 0)
		return errno;

	return 0;
}
