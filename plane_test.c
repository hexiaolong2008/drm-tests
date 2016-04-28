/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>

#include "bs_drm.h"

#define MAX_PLANES 4
#define MAX_COMPONENTS 4

static drmModeModeInfoPtr find_best_mode(int mode_count, drmModeModeInfoPtr modes)
{
	if (mode_count <= 0 || modes == NULL)
		return NULL;

	for (int m = 0; m < mode_count; m++)
		if (modes[m].type & DRM_MODE_TYPE_PREFERRED)
			return &modes[m];

	return &modes[0];
}

static bool is_format_supported(uint32_t format_count, uint32_t *formats, uint32_t format)
{
	for (uint32_t i = 0; i < format_count; i++)
		if (formats[i] == format)
			return true;
	return false;
}

static uint32_t find_overlay_plane(int fd, uint32_t crtc_id, uint32_t format)
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

		if ((plane->possible_crtcs & crtc_mask) == 0 ||
		    !is_format_supported(plane->count_formats, plane->formats, format)) {
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

struct draw_format_component {
	float rgb_coeffs[3];
	float value_offset;
	uint32_t horizontal_subsample_rate;
	uint32_t vertical_subsample_rate;
	uint32_t pixel_skip;
	uint32_t plane_index;
	uint32_t plane_offset;
};

struct draw_format {
	uint32_t pixel_format;
	const char *name;
	size_t component_count;
	struct draw_format_component components[MAX_COMPONENTS];
};

#define PIXEL_FORMAT_AND_NAME(x) GBM_FORMAT_##x, #x

static const struct draw_format g_draw_formats[] = {
	{
	    PIXEL_FORMAT_AND_NAME(NV12),
	    3,
	    {
		{ { 0.2567890625f, 0.50412890625f, 0.09790625f }, 16.0f, 1, 1, 1, 0, 0 },
		{ { -0.14822265625f, -0.2909921875f, 0.43921484375f }, 128.0f, 2, 2, 1, 1, 0 },
		{ { 0.43921484375f, -0.3677890625f, -0.07142578125f }, 128.0f, 2, 2, 1, 1, 1 },
	    },
	},
	{
	    PIXEL_FORMAT_AND_NAME(XRGB8888),
	    3,
	    {
		{ { 0.0f, 0.0f, 1.0f }, 0.0f, 1, 1, 4, 0, 0 },
		{ { 0.0f, 1.0f, 0.0f }, 0.0f, 1, 1, 4, 0, 1 },
		{ { 1.0f, 0.0f, 0.0f }, 0.0f, 1, 1, 4, 0, 2 },
	    },
	},
	{
	    PIXEL_FORMAT_AND_NAME(ARGB8888),
	    4,
	    {
		{ { 0.0f, 0.0f, 1.0f }, 0.0f, 1, 1, 4, 0, 0 },
		{ { 0.0f, 1.0f, 0.0f }, 0.0f, 1, 1, 4, 0, 1 },
		{ { 1.0f, 0.0f, 0.0f }, 0.0f, 1, 1, 4, 0, 2 },
		{ { 0.0f, 0.0f, 0.0f }, 255.0f, 1, 1, 4, 0, 3 },
	    },
	},
};

static uint8_t clampbyte(float f)
{
	if (f >= 255.0f)
		return 255;
	if (f <= 0.0f)
		return 0;
	return (uint8_t)f;
}

static uint8_t convert_color(const struct draw_format_component *comp, uint8_t r, uint8_t g,
			     uint8_t b)
{
	return clampbyte(comp->value_offset + r * comp->rgb_coeffs[0] + g * comp->rgb_coeffs[1] +
			 b * comp->rgb_coeffs[2]);
}

static const struct draw_format *get_draw_format(uint32_t pixel_format)
{
	for (size_t format_index = 0;
	     format_index < sizeof(g_draw_formats) / sizeof(g_draw_formats[0]); format_index++) {
		const struct draw_format *format = &g_draw_formats[format_index];
		if (format->pixel_format == pixel_format)
			return format;
	}

	return NULL;
}

struct draw_plane {
	uint32_t row_stride;
	uint8_t *ptr;
};

static void unmmap_planes(struct gbm_bo *bo, size_t num_planes, struct draw_plane *planes)
{
	for (uint32_t plane_index = 0; plane_index < num_planes; plane_index++)
		bs_dma_buf_unmmap_plane(bo, plane_index, planes[plane_index].ptr);
}

static size_t mmap_planes(struct gbm_bo *bo, struct draw_plane planes[MAX_PLANES])
{
	size_t num_planes = gbm_bo_get_num_planes(bo);
	if (num_planes > MAX_PLANES) {
		bs_debug_error("buffer object has unexpected number of planes %zu", num_planes);
		return 0;
	}

	for (size_t plane_index = 0; plane_index < num_planes; plane_index++) {
		struct draw_plane *plane = &planes[plane_index];
		plane->row_stride = gbm_bo_get_plane_stride(bo, plane_index);
		plane->ptr = bs_dma_buf_mmap_plane(bo, plane_index);
		if (!plane->ptr) {
			bs_debug_error("failed to mmap plane %zu of buffer object", plane_index);
			unmmap_planes(bo, plane_index, planes);
			return 0;
		}
	}

	return true;
}

static bool draw_pattern(struct gbm_bo *bo, const struct draw_format *format)
{
	const uint32_t width = gbm_bo_get_width(bo);
	const uint32_t height = gbm_bo_get_height(bo);
	const uint32_t striph = height / 4;

	struct draw_plane planes[MAX_PLANES];
	size_t num_planes = mmap_planes(bo, planes);
	if (num_planes == 0) {
		bs_debug_error("failed to prepare to draw pattern to buffer object");
		return false;
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
				r = g = b = 0;
				break;
		}
		for (uint32_t y = s * striph; y < (s + 1) * striph; y++) {
			uint8_t *rows[MAX_COMPONENTS] = { 0 };
			for (size_t comp_index = 0; comp_index < format->component_count;
			     comp_index++) {
				const struct draw_format_component *comp =
				    &format->components[comp_index];
				struct draw_plane *plane = &planes[comp->plane_index];
				rows[comp_index] =
				    plane->ptr + comp->plane_offset +
				    plane->row_stride * (y / comp->vertical_subsample_rate);
			}
			for (uint32_t x = 0; x < width; x++) {
				const float i = (float)x / (float)width * 256.0f;
				for (size_t comp_index = 0; comp_index < format->component_count;
				     comp_index++) {
					const struct draw_format_component *comp =
					    &format->components[comp_index];
					if ((y % comp->vertical_subsample_rate) == 0 &&
					    (x % comp->horizontal_subsample_rate) == 0)
						*(rows[comp_index] + x * comp->pixel_skip) =
						    convert_color(comp, r * i, g * i, b * i);
				}
			}
		}
	}

	unmmap_planes(bo, num_planes, planes);
	return true;
}

int main(int argc, char **argv)
{
	assert(sizeof(g_draw_formats) / sizeof(g_draw_formats[0]) > 0);
	const struct draw_format *plane_format = &g_draw_formats[0];
	if (argc == 2) {
		char *format_str = argv[1];
		if (strlen(format_str) == 4) {
			plane_format = get_draw_format(*(uint32_t *)format_str);
		}
		else {
			plane_format = 0;
			for (size_t format_index = 0;
			     format_index < sizeof(g_draw_formats) / sizeof(g_draw_formats[0]);
			     format_index++) {
				const struct draw_format *format = &g_draw_formats[format_index];
				if (!strcmp(format_str, format->name)) {
					plane_format = format;
					break;
				}
			}
		}

		if (plane_format == NULL) {
			printf("plane format %s is not recognized\n", format_str);
			return false;
		}
	}

	printf("Using plane format %s (%4.4s)\n", plane_format->name,
	       (char *)&plane_format->pixel_format);

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

	uint32_t plane_id = find_overlay_plane(pipe.fd, pipe.crtc_id, plane_format->pixel_format);
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

	struct gbm_bo *bg_bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
					     GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
	if (!bg_bo) {
		bs_debug_error("failed to create background buffer ojbect");
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
	struct gbm_bo *bo =
	    gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay, plane_format->pixel_format,
			  GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
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

	if (!draw_pattern(bo, plane_format)) {
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
