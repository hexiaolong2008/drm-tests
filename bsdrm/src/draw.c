/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.

 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

struct draw_format_component {
	float rgb_coeffs[3];
	float value_offset;
	uint32_t horizontal_subsample_rate;
	uint32_t vertical_subsample_rate;
	uint32_t pixel_skip;
	uint32_t plane_index;
	uint32_t plane_offset;
};

#define MAX_COMPONENTS 4
struct bs_draw_format {
	uint32_t pixel_format;
	const char *name;
	size_t component_count;
	struct draw_format_component components[MAX_COMPONENTS];
};

#define PIXEL_FORMAT_AND_NAME(x) GBM_FORMAT_##x, #x
static const struct bs_draw_format bs_draw_formats[] = {
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
	    PIXEL_FORMAT_AND_NAME(YVU420),
	    3,
	    {
		{ { 0.2567890625f, 0.50412890625f, 0.09790625f }, 16.0f, 1, 1, 1, 0, 0 },
		{ { 0.43921484375f, -0.3677890625f, -0.07142578125f }, 128.0f, 2, 2, 1, 1, 0 },
		{ { -0.14822265625f, -0.2909921875f, 0.43921484375f }, 128.0f, 2, 2, 1, 2, 0 },
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
	{
	    PIXEL_FORMAT_AND_NAME(YUYV),
	    4,
	    {
		{ { 0.43921484375f, -0.3677890625f, -0.07142578125f }, 128.0f, 1, 1, 4, 0, 0 },
		{ { 0.2567890625f, 0.50412890625f, 0.09790625f }, 16.0f, 1, 1, 4, 0, 1 },
		{ { -0.14822265625f, -0.2909921875f, 0.43921484375f }, 128.0f, 1, 1, 4, 0, 2 },
		{ { 0.2567890625f, 0.50412890625f, 0.09790625f }, 16.0f, 1, 1, 4, 0, 3 },
	    },
	},
};

struct draw_plane {
	uint32_t row_stride;
	uint8_t *ptr;
	void *map_data;
};

static uint8_t clampbyte(float f)
{
	if (f >= 255.0f)
		return 255;
	if (f <= 0.0f)
		return 0;
	return (uint8_t)f;
}

uint8_t static convert_color(const struct draw_format_component *comp, uint8_t r, uint8_t g,
			     uint8_t b)
{
	return clampbyte(comp->value_offset + r * comp->rgb_coeffs[0] + g * comp->rgb_coeffs[1] +
			 b * comp->rgb_coeffs[2]);
}

static void unmmap_planes(struct bs_mapper *mapper, struct gbm_bo *bo, size_t num_planes,
			  struct draw_plane *planes)
{
	for (uint32_t plane_index = 0; plane_index < num_planes; plane_index++)
		bs_mapper_unmap(mapper, bo, planes[plane_index].map_data);
}

static size_t mmap_planes(struct bs_mapper *mapper, struct gbm_bo *bo,
			  struct draw_plane planes[GBM_MAX_PLANES])
{
	size_t num_planes = gbm_bo_get_num_planes(bo);
	for (size_t plane_index = 0; plane_index < num_planes; plane_index++) {
		struct draw_plane *plane = &planes[plane_index];
		plane->row_stride = gbm_bo_get_plane_stride(bo, plane_index);
		plane->ptr = bs_mapper_map(mapper, bo, plane_index, &plane->map_data);
		if (plane->ptr == MAP_FAILED) {
			bs_debug_error("failed to mmap plane %zu of buffer object", plane_index);
			unmmap_planes(mapper, bo, plane_index, planes);
			return 0;
		}
	}

	return num_planes;
}

bool bs_draw_pattern(struct bs_mapper *mapper, struct gbm_bo *bo,
		     const struct bs_draw_format *format)
{
	const uint32_t width = gbm_bo_get_width(bo);
	const uint32_t height = gbm_bo_get_height(bo);
	const uint32_t striph = height / 4;

	struct draw_plane planes[GBM_MAX_PLANES];
	size_t num_planes = mmap_planes(mapper, bo, planes);
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

	unmmap_planes(mapper, bo, num_planes, planes);
	return true;
}

const struct bs_draw_format *bs_get_draw_format(uint32_t pixel_format)
{
	for (size_t format_index = 0; format_index < BS_ARRAY_LEN(bs_draw_formats);
	     format_index++) {
		const struct bs_draw_format *format = &bs_draw_formats[format_index];
		if (format->pixel_format == pixel_format)
			return format;
	}

	return NULL;
}

const struct bs_draw_format *bs_get_draw_format_from_name(const char *str)
{
	for (size_t format_index = 0; format_index < BS_ARRAY_LEN(bs_draw_formats);
	     format_index++) {
		const struct bs_draw_format *format = &bs_draw_formats[format_index];
		if (!strcmp(str, format->name))
			return format;
	}

	return NULL;
}

uint32_t bs_get_pixel_format(const struct bs_draw_format *format)
{
	assert(format);
	return format->pixel_format;
}

const char *bs_get_format_name(const struct bs_draw_format *format)
{
	assert(format);
	return format->name;
}
