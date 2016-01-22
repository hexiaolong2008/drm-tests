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
		fprintf(stderr, "failed to open card for display\n");
		return 1;
	}

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "failed to create gbm\n");
		return 1;
	}

	struct bs_drm_pipe pipe = {0};
	if (!bs_drm_pipe_make(fd, &pipe)) {
		fprintf(stderr, "failed to make pipe\n");
		return 1;
	}

	drmModeConnector *connector = drmModeGetConnector(fd, pipe.connector_id);
	drmModeModeInfo *mode = &connector->modes[0];

	struct bs_drm_fb *fbs[2];
	for (size_t fb_index = 0; fb_index < 2; fb_index++) {
		fbs[fb_index] =
		    bs_drm_fb_new(gbm, mode->hdisplay, mode->vdisplay, GBM_FORMAT_XRGB8888,
				  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (fbs[fb_index] == NULL) {
			fprintf(stderr, "failed to make swap chain\n");
			return 1;
		}
	}

	for (size_t frame_index = 0; frame_index < 10000; frame_index++) {
		size_t fb_index = frame_index % 2;
		struct gbm_bo *bo = bs_drm_fb_bo(fbs[fb_index]);
		size_t bo_size = gbm_bo_get_stride(bo) * mode->vdisplay;
		char *ptr = bs_dumb_mmap_gbm(bo);
		for (size_t i = 0; i < bo_size / 4; i++) {
			ptr[i * 4 + 0] = (i + frame_index * 50) % 256;
			ptr[i * 4 + 1] = (i + frame_index * 50 + 85) % 256;
			ptr[i * 4 + 2] = (i + frame_index * 50 + 170) % 256;
			ptr[i * 4 + 3] = 0;
		}
		bs_dumb_unmmap_gbm(bo, ptr);

		int ret =
		    drmModeSetCrtc(fd, pipe.crtc_id, bs_drm_fb_id(fbs[fb_index]), 0 /* x */,
				   0 /* y */, &pipe.connector_id, 1 /* connector count */, mode);
		if (ret) {
			fprintf(stderr, "failed to set crtc: %d\n", ret);
			return 1;
		}
		usleep(16667);
	}

	for (size_t fb_index = 0; fb_index < 2; fb_index++)
		bs_drm_fb_destroy(&fbs[fb_index]);

	return 0;
}
