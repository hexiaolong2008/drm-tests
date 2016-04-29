/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

void *bs_dma_buf_mmap(struct gbm_bo *bo)
{
	return bs_dma_buf_mmap_plane(bo, 0);
}

void *bs_dma_buf_mmap_plane(struct gbm_bo *bo, size_t plane)
{
	assert(bo);

	int drm_prime_fd = gbm_bo_get_plane_fd(bo, plane);

	void *addr = mmap(NULL, gbm_bo_get_plane_size(bo, plane), (PROT_READ | PROT_WRITE),
			  MAP_SHARED, drm_prime_fd, 0);
	if (addr == MAP_FAILED) {
		bs_debug_error("mmap returned MAP_FAILED: %d", errno);
		addr = NULL;
	} else {
		addr += gbm_bo_get_plane_offset(bo, plane);
	}

	close(drm_prime_fd);

	return addr;
}

int bs_dma_buf_unmmap(struct gbm_bo *bo, void *addr)
{
	return bs_dma_buf_unmmap_plane(bo, 0, addr);
}

int bs_dma_buf_unmmap_plane(struct gbm_bo *bo, size_t plane, void *addr)
{
	assert(bo);
	assert(addr != NULL);
	assert(addr != MAP_FAILED);
	assert(addr >= (void *)(uintptr_t)gbm_bo_get_plane_offset(bo, plane));

	addr -= gbm_bo_get_plane_offset(bo, plane);
	int ret = munmap(addr, gbm_bo_get_plane_size(bo, plane));

	if (ret != 0)
		return errno;

	return 0;
}
