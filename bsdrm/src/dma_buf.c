/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

void *bs_dma_buf_mmap(struct gbm_bo *bo)
{
	assert(bo);

	int drm_prime_fd = gbm_bo_get_fd(bo);
	size_t size = gbm_bo_get_stride(bo) * gbm_bo_get_height(bo);

	void *addr = mmap(NULL, size, (PROT_READ | PROT_WRITE), MAP_SHARED, drm_prime_fd, 0);
	if (addr == MAP_FAILED) {
		bs_debug_error("mmap returned MAP_FAILED: %d", errno);
		return NULL;
	}

	close(drm_prime_fd);

	return addr;
}

int bs_dma_buf_unmmap(struct gbm_bo *bo, void *addr)
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
