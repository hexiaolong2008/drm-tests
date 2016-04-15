/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

static drmModeModeInfoPtr find_best_mode(int mode_count, drmModeModeInfoPtr modes)
{
	if (mode_count <= 0 || modes == NULL)
		return NULL;

	for (int m = 0; m < mode_count; m++)
		if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
			return &modes[m];

	return &modes[0];
}

static uint32_t find_overlay_plane(int fd, uint32_t crtc_id)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (res == NULL) {
		bs_debug_error("failed to get drm resources");
		return 0;
	}

	uint32_t crtc_mask = 0;
	for (int crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++) {
		if (res->crtcs[crtc_index] == crtc_id) {
			crtc_mask = (1 << crtc_index);
			break;
		}
	}
	if (crtc_mask == 0) {
		bs_debug_error("invalid crtc id %u", crtc_id);
		drmModeFreeResources(res);
		return 0;
	}

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
	if (plane_res == NULL) {
		bs_debug_error("failed to get plane resources");
		drmModeFreeResources(res);
		return 0;
	}

	uint32_t plane_id = 0;
	bool use_plane = false;
	for (uint32_t plane_index = 0; !use_plane && plane_index < plane_res->count_planes;
	     plane_index++) {
		drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[plane_index]);
		if (plane == NULL) {
			bs_debug_error("failed to get plane id %u", plane_res->planes[plane_index]);
			continue;
		}

		if ((plane->possible_crtcs & crtc_mask) == 0) {
			drmModeFreePlane(plane);
			continue;
		}

		drmModeObjectPropertiesPtr props =
		    drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
		for (uint32_t prop_index = 0; prop_index < props->count_props; ++prop_index) {
			drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[prop_index]);
			if (strcmp(prop->name, "type")) {
				drmModeFreeProperty(prop);
				continue;
			}

			uint64_t desired_value = 0;
			bool has_value = false;
			for (int enum_index = 0; enum_index < prop->count_enums; enum_index++) {
				struct drm_mode_property_enum *penum = &prop->enums[enum_index];
				if (!strcmp(penum->name, "Overlay")) {
					desired_value = penum->value;
					has_value = true;
					break;
				}
			}
			drmModeFreeProperty(prop);

			if (has_value && desired_value == props->prop_values[prop_index])
				use_plane = true;

			break;
		}
		drmModeFreeObjectProperties(props);

		if (use_plane)
			plane_id = plane->plane_id;

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(res);

	return plane_id;
}

static uint8_t clampbyte(double f)
{
	if (f >= 255.0)
		return 255;
	if (f <= 0.0)
		return 0;
	return (uint8_t)f;
}

static uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b)
{
	return clampbyte(16 + 0.2567890625 * r + 0.50412890625 * g + 0.09790625 * b);
}

static uint8_t rgb_to_cb(uint8_t r, uint8_t g, uint8_t b)
{
	return clampbyte(128 - 0.14822265625 * r - 0.2909921875 * g + 0.43921484375 * b);
}

static uint8_t rgb_to_cr(uint8_t r, uint8_t g, uint8_t b)
{
	return clampbyte(128 + 0.43921484375 * r - 0.3677890625 * g - 0.07142578125 * b);
}

static bool draw_pattern(struct gbm_bo *bo)
{
	const uint32_t stride = gbm_bo_get_stride(bo);
	const uint32_t width = gbm_bo_get_width(bo);
	const uint32_t height = gbm_bo_get_height(bo);
	const uint32_t striph = height / 4;

	uint8_t *y_ptr = bs_dma_buf_mmap_plane(bo, 0);
	if (!y_ptr) {
		bs_debug_error("failed to mmap y plane buffer while drawing pattern");
		return false;
	}

	uint8_t *uv_ptr = bs_dma_buf_mmap_plane(bo, 1);
	if (!uv_ptr) {
		bs_debug_error("failed to mmap uv plane buffer while drawing pattern");
		bs_dma_buf_unmmap_plane(bo, 0, y_ptr);
		return false;
	}

	bool success = true;

	for (uint32_t y = 0; y < height; y++) {
		uint8_t *const yrow = y_ptr + y * stride;
		for (uint32_t x = 0; x < width; x++)
			yrow[x] = 16;
	}
	for (uint32_t y = 0; y < height / 2; y++) {
		uint8_t *const uvrow = uv_ptr + y * stride;
		for (uint32_t x = 0; x < width; x++)
			uvrow[x] = 128;
	}

	for (uint32_t s = 0; s < 4; s++) {
		uint8_t r = 0, g = 0, b = 0;
		switch (s) {
			case 0:
				r = g = b = 1;
				break;
			case 1:
				r = 1;
				break;
			case 2:
				g = 1;
				break;
			case 3:
				b = 1;
				break;
			default:
				assert("invalid strip" && false);
				success = false;
				goto out;
		}
		for (uint32_t y = s * striph; y < (s + 1) * striph; y++) {
			uint8_t *const yrow = y_ptr + y * stride;
			uint8_t *const uvrow = uv_ptr + (y / 2) * stride;
			for (uint32_t x = 0; x < width; x++) {
				const float i = (float)x / (float)width * 256.0f;
				yrow[x] = rgb_to_y(r * i, g * i, b * i);
				if ((y % 2) == 0 && (x % 2) == 0) {
					uvrow[x + 0] = rgb_to_cb(r * i, g * i, b * i);
					uvrow[x + 1] = rgb_to_cr(r * i, g * i, b * i);
				}
			}
		}
	}

out:
	bs_dma_buf_unmmap_plane(bo, 1, uv_ptr);
	bs_dma_buf_unmmap_plane(bo, 0, y_ptr);
	return success;
}

int main()
{
	drmModeConnector *connector;
	struct bs_drm_pipe pipe = { 0 };
	struct bs_drm_pipe_plumber *plumber = bs_drm_pipe_plumber_new();
	bs_drm_pipe_plumber_connector_ranks(plumber, bs_drm_connectors_internal_rank);
	bs_drm_pipe_plumber_connector_ptr(plumber, &connector);
	if (!bs_drm_pipe_plumber_make(plumber, &pipe)) {
		bs_debug_error("failed to make pipe");
		return 1;
	}
	bs_drm_pipe_plumber_destroy(&plumber);

	drmModeModeInfo *mode_ptr = find_best_mode(connector->count_modes, connector->modes);
	if (!mode_ptr) {
		bs_debug_error("failed to find preferred mode");
		return 1;
	}
	drmModeModeInfo mode = *mode_ptr;
	drmModeFreeConnector(connector);
	printf("Using mode %s\n", mode.name);

	uint32_t plane_id = find_overlay_plane(pipe.fd, pipe.crtc_id);
	if (plane_id == 0) {
		bs_debug_error("failed to find overlay plane");
		return 1;
	}

	printf("Using CRTC:%u ENCODER:%u CONNECTOR:%u PLANE:%u\n", pipe.crtc_id, pipe.encoder_id,
	       pipe.connector_id, plane_id);

	struct gbm_device *gbm = gbm_create_device(pipe.fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm");
		return 1;
	}

	struct gbm_bo *bg_bo =
	    gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888, 0);
	if (!bg_bo) {
		bs_debug_error("failed to create background buffer object");
		return 1;
	}

	void *bo_ptr = bs_dma_buf_mmap_plane(bg_bo, 0);
	if (bo_ptr == NULL) {
		bs_debug_error("failed to mmap background buffer object");
		return 1;
	}
	memset(bo_ptr, 0xff, gbm_bo_get_height(bg_bo) * gbm_bo_get_stride(bg_bo));
	bs_dma_buf_unmmap_plane(bg_bo, 0, bo_ptr);

	printf("Creating buffer %ux%u\n", mode.hdisplay, mode.vdisplay);
	struct gbm_bo *bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_NV12, 0);
	if (!bo) {
		bs_debug_error("failed to create buffer object");
		return 1;
	}

	uint32_t crtc_fb_id = bs_drm_fb_create_gbm(bg_bo);
	if (!crtc_fb_id) {
		bs_debug_error("failed to create frame buffer for buffer object");
		return 1;
	}

	uint32_t plane_fb_id = bs_drm_fb_create_gbm(bo);
	if (!plane_fb_id) {
		bs_debug_error("failed to create plane frame buffer for buffer object");
		return 1;
	}

	if (!draw_pattern(bo)) {
		bs_debug_error("failed to draw pattern to buffer object");
		return 1;
	}

	int ret =
	    drmModeSetCrtc(pipe.fd, pipe.crtc_id, crtc_fb_id, 0, 0, &pipe.connector_id, 1, &mode);
	if (ret < 0) {
		bs_debug_error("Could not set mode on CRTC %d %s", pipe.crtc_id, strerror(errno));
		return 1;
	}

	ret = drmModeSetPlane(pipe.fd, plane_id, pipe.crtc_id, plane_fb_id, 0, 0, 0, mode.hdisplay,
			      mode.vdisplay, 0, 0, mode.hdisplay << 16, mode.vdisplay << 16);

	if (ret) {
		bs_debug_error("failed to set plane %d", ret);
		return 1;
	}

	sleep(5);

	ret = drmModeSetCrtc(pipe.fd, pipe.crtc_id, 0, 0, 0, NULL, 0, NULL);
	if (ret < 0) {
		bs_debug_error("Could not disable CRTC %d %s", pipe.crtc_id, strerror(errno));
		return 1;
	}

	drmModeRmFB(pipe.fd, plane_fb_id);
	drmModeRmFB(pipe.fd, crtc_fb_id);
	gbm_bo_destroy(bo);
	gbm_bo_destroy(bg_bo);
	gbm_device_destroy(gbm);
	close(pipe.fd);

	return 0;
}
