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

struct draw_data {
	uint32_t x;
	uint32_t y;
	uint32_t w;
	uint32_t h;
	float progress;
	uint8_t out_color[MAX_COMPONENTS];
};

typedef void (*compute_color_t)(struct draw_data *data);

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
	    3,
	    {
		{ { 0.2567890625f, 0.50412890625f, 0.09790625f }, 16.0f, 1, 1, 2, 0, 0 },
		{ { -0.14822265625f, -0.2909921875f, 0.43921484375f }, 128.0f, 2, 1, 4, 0, 1 },
		{ { 0.43921484375f, -0.3677890625f, -0.07142578125f }, 128.0f, 2, 1, 4, 0, 3 },
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

static bool draw_color(struct bs_mapper *mapper, struct gbm_bo *bo,
		       const struct bs_draw_format *format, struct draw_data *data,
		       compute_color_t compute_color_fn)
{
	uint8_t *ptr, *converted_colors[MAX_COMPONENTS];
	struct draw_plane planes[GBM_MAX_PLANES];
	uint32_t height = data->h = gbm_bo_get_height(bo);
	uint32_t width = data->w = gbm_bo_get_width(bo);

	size_t num_planes = mmap_planes(mapper, bo, planes);
	if (num_planes == 0) {
		bs_debug_error("failed to prepare to draw pattern to buffer object");
		return false;
	}

	for (size_t comp_index = 0; comp_index < format->component_count; comp_index++) {
		converted_colors[comp_index] = calloc(width * height, sizeof(uint8_t));
		assert(converted_colors[comp_index]);
	}

	for (uint32_t y = 0; y < height; y++) {
		data->y = y;
		for (uint32_t x = 0; x < width; x++) {
			data->x = x;
			compute_color_fn(data);
			for (size_t comp_index = 0; comp_index < format->component_count;
			     comp_index++) {
				const struct draw_format_component *comp =
				    &format->components[comp_index];
				ptr = converted_colors[comp_index] + width * y + x;
				*ptr = convert_color(comp, data->out_color[2], data->out_color[1],
						     data->out_color[0]);
			}
		}
	}

	uint32_t color, samples, offset;
	uint8_t *rows[MAX_COMPONENTS] = { 0 };
	for (size_t comp_index = 0; comp_index < format->component_count; comp_index++) {
		const struct draw_format_component *comp = &format->components[comp_index];
		struct draw_plane *plane = &planes[comp->plane_index];
		for (uint32_t y = 0; y < height / comp->vertical_subsample_rate; y++) {
			rows[comp_index] = plane->ptr + comp->plane_offset + plane->row_stride * y;
			for (uint32_t x = 0; x < width / comp->horizontal_subsample_rate; x++) {
				offset = color = samples = 0;
				for (uint32_t j = 0; j < comp->vertical_subsample_rate; j++) {
					offset = (y * comp->vertical_subsample_rate + j) * width +
						 x * comp->horizontal_subsample_rate;
					for (uint32_t i = 0; i < comp->horizontal_subsample_rate;
					     i++) {
						color += converted_colors[comp_index][offset];
						samples++;
						offset++;
					}
				}

				*(rows[comp_index] + x * comp->pixel_skip) = color / samples;
			}
		}
	}

	unmmap_planes(mapper, bo, num_planes, planes);
	for (size_t comp_index = 0; comp_index < format->component_count; comp_index++) {
		free(converted_colors[comp_index]);
	}

	return true;
}

static void compute_stripe(struct draw_data *data)
{
	const uint32_t striph = data->h / 4;
	const uint32_t s = data->y / striph;
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

	const float i = (float)data->x / (float)data->w * 256.0f;
	data->out_color[0] = b * i;
	data->out_color[1] = g * i;
	data->out_color[2] = r * i;
	data->out_color[3] = 0;
}

static void compute_ellipse(struct draw_data *data)
{
	float xratio = ((int)data->x - (int)data->w / 2) / ((float)(data->w / 2));
	float yratio = ((int)data->y - (int)data->h / 2) / ((float)(data->h / 2));

	// If a point is on or inside an ellipse, num <= 1.
	float num = xratio * xratio + yratio * yratio;
	uint32_t g = 255 * num;

	if (g < 256) {
		memset(data->out_color, 0, 4);
		data->out_color[2] = 0xFF;
		data->out_color[1] = g;
	} else {
		memset(data->out_color, (uint8_t)(data->progress * 255), 4);
	}
}

bool bs_draw_stripe(struct bs_mapper *mapper, struct gbm_bo *bo,
		    const struct bs_draw_format *format)
{
	struct draw_data data = { 0 };
	return draw_color(mapper, bo, format, &data, compute_stripe);
}

bool bs_draw_ellipse(struct bs_mapper *mapper, struct gbm_bo *bo,
		     const struct bs_draw_format *format, float progress)
{
	struct draw_data data = { 0 };
	data.progress = progress;
	return draw_color(mapper, bo, format, &data, compute_ellipse);
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

bool bs_parse_draw_format(const char *str, const struct bs_draw_format **format)
{
	if (strlen(str) == 4) {
		const struct bs_draw_format *bs_draw_format = bs_get_draw_format(*(uint32_t *)str);
		if (bs_draw_format) {
			*format = bs_draw_format;
			return true;
		}
	} else {
		const struct bs_draw_format *bs_draw_format = bs_get_draw_format_from_name(str);
		if (bs_draw_format) {
			*format = bs_draw_format;
			return true;
		}
	}

	bs_debug_error("format %s is not recognized\n", str);
	return false;
}
