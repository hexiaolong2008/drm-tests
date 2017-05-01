/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This file performs some sanity checks on the DRM atomic API. To run a test, please run the
 * following command:
 *
 * atomictest <testname>
 *
 * To get a list of possible tests, run:
 *
 * atomictest
 */

#include "bs_drm.h"

#define CHECK(cond)                                               \
	do {                                                      \
		if (!(cond)) {                                    \
			bs_debug_error("check %s failed", #cond); \
			return -1;                                \
		}                                                 \
	} while (0)

#define CHECK_RESULT(ret)                                             \
	do {                                                          \
		if ((ret) < 0) {                                      \
			bs_debug_error("failed with error: %d", ret); \
			return -1;                                    \
		}                                                     \
	} while (0)

#define CURSOR_SIZE 64

static const uint32_t yuv_formats[] = {
	DRM_FORMAT_NV12, DRM_FORMAT_UYVY, DRM_FORMAT_YUYV, DRM_FORMAT_YVU420,
};

static struct gbm_device *gbm = NULL;

static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
			      unsigned int tv_usec, void *user_data)
{
	// Nothing to do.
}

struct atomictest_property {
	uint32_t pid;
	uint32_t value;
};

struct atomictest_plane {
	drmModePlane drm_plane;
	struct gbm_bo *bo;

	uint32_t format_idx;

	/* Properties. */
	struct atomictest_property crtc_id;
	struct atomictest_property crtc_x;
	struct atomictest_property crtc_y;
	struct atomictest_property crtc_w;
	struct atomictest_property crtc_h;
	struct atomictest_property fb_id;
	struct atomictest_property src_x;
	struct atomictest_property src_y;
	struct atomictest_property src_w;
	struct atomictest_property src_h;
	struct atomictest_property type;
	struct atomictest_property zpos;
};

struct atomictest_connector {
	uint32_t connector_id;
	struct atomictest_property crtc_id;
	struct atomictest_property edid;
	struct atomictest_property dpms;
};

struct atomictest_crtc {
	uint32_t crtc_id;
	uint32_t width;
	uint32_t height;
	uint32_t *primary_idx;
	uint32_t *cursor_idx;
	uint32_t *overlay_idx;
	uint32_t num_primary;
	uint32_t num_cursor;
	uint32_t num_overlay;

	struct atomictest_plane *planes;
	struct atomictest_property mode_id;
	struct atomictest_property active;
};

struct atomictest_mode {
	uint32_t height;
	uint32_t width;
	uint32_t id;
};

struct atomictest_context {
	int fd;
	uint32_t num_crtcs;
	uint32_t num_connectors;
	uint32_t num_modes;

	struct atomictest_connector *connectors;
	struct atomictest_crtc *crtcs;
	struct atomictest_mode *modes;
	drmModeAtomicReqPtr pset;
	drmEventContext drm_event_ctx;

	struct bs_mapper *mapper;
};

struct atomictest {
	const char *name;
	int (*run_test)(struct atomictest_context *ctx, struct atomictest_crtc *crtc);
};

static int32_t get_format_idx(struct atomictest_plane *plane, uint32_t format)
{
	for (int32_t i = 0; i < plane->drm_plane.count_formats; i++)
		if (plane->drm_plane.formats[i] == format)
			return i;
	return -1;
}

static void copy_drm_plane(drmModePlane *dest, drmModePlane *src)
{
	memcpy(dest, src, sizeof(drmModePlane));
	dest->formats = calloc(src->count_formats, sizeof(uint32_t));
	dest->format_modifiers =
	    calloc(src->count_format_modifiers, sizeof(struct drm_format_modifier));
	memcpy(dest->formats, src->formats, src->count_formats * sizeof(uint32_t));
	memcpy(dest->format_modifiers, src->format_modifiers,
	       src->count_format_modifiers * sizeof(struct drm_format_modifier));
}

static struct atomictest_plane *get_plane(struct atomictest_crtc *crtc, uint32_t idx, uint64_t type)
{
	uint32_t index;
	switch (type) {
		case DRM_PLANE_TYPE_OVERLAY:
			index = crtc->overlay_idx[idx];
			break;
		case DRM_PLANE_TYPE_PRIMARY:
			index = crtc->primary_idx[idx];
			break;
		case DRM_PLANE_TYPE_CURSOR:
			index = crtc->cursor_idx[idx];
			break;
		default:
			bs_debug_error("invalid plane type returned");
			return NULL;
	}

	return &crtc->planes[index];
}

static void write_to_buffer(struct bs_mapper *mapper, struct gbm_bo *bo, uint32_t u32, uint16_t u16)
{
	void *map_data;
	uint32_t num_ints;
	uint32_t format = gbm_bo_get_format(bo);
	void *addr = bs_mapper_map(mapper, bo, 0, &map_data);

	if (format == GBM_FORMAT_RGB565 || format == GBM_FORMAT_BGR565) {
		num_ints = gbm_bo_get_plane_size(bo, 0) / sizeof(uint16_t);
		uint16_t *pixel = (uint16_t *)addr;
		for (uint32_t i = 0; i < num_ints; i++)
			pixel[i] = u16;
	} else {
		num_ints = gbm_bo_get_plane_size(bo, 0) / sizeof(uint32_t);
		uint32_t *pixel = (uint32_t *)addr;
		for (uint32_t i = 0; i < num_ints; i++)
			pixel[i] = u32;
	}

	bs_mapper_unmap(mapper, bo, map_data);
}

static void draw_cursor(struct bs_mapper *mapper, struct gbm_bo *bo)
{
	void *map_data;
	uint32_t *cursor_ptr = bs_mapper_map(mapper, bo, 0, &map_data);
	for (size_t y = 0; y < gbm_bo_get_height(bo); y++) {
		for (size_t x = 0; x < gbm_bo_get_width(bo); x++) {
			// A white triangle pointing right
			bool color_white = y > x / 2 && y < (gbm_bo_get_width(bo) - x / 2);
			cursor_ptr[y * gbm_bo_get_height(bo) + x] =
			    (color_white) ? 0xFFFFFFFF : 0x00000000;
		}
	}

	bs_mapper_unmap(mapper, bo, map_data);
}

static int get_prop(int fd, drmModeObjectPropertiesPtr props, const char *name,
		    struct atomictest_property *bs_prop)
{
	/* Property ID should always be > 0. */
	bs_prop->pid = 0;
	drmModePropertyPtr prop;
	for (uint32_t i = 0; i < props->count_props; i++) {
		if (bs_prop->pid)
			break;

		prop = drmModeGetProperty(fd, props->props[i]);
		if (prop) {
			if (!strcmp(prop->name, name)) {
				bs_prop->pid = prop->prop_id;
				bs_prop->value = props->prop_values[i];
			}
			drmModeFreeProperty(prop);
		}
	}

	return (bs_prop->pid == 0) ? -1 : 0;
}

static int get_connector_props(int fd, struct atomictest_connector *connector,
			       drmModeObjectPropertiesPtr props)
{
	CHECK_RESULT(get_prop(fd, props, "EDID", &connector->edid));
	CHECK_RESULT(get_prop(fd, props, "DPMS", &connector->dpms));
	return 0;
}

static int get_crtc_props(int fd, struct atomictest_crtc *crtc, drmModeObjectPropertiesPtr props)
{
	CHECK_RESULT(get_prop(fd, props, "MODE_ID", &crtc->mode_id));
	CHECK_RESULT(get_prop(fd, props, "ACTIVE", &crtc->active));
	return 0;
}

static int get_plane_props(int fd, struct atomictest_plane *plane, drmModeObjectPropertiesPtr props)
{
	CHECK_RESULT(get_prop(fd, props, "CRTC_ID", &plane->crtc_id));
	CHECK_RESULT(get_prop(fd, props, "FB_ID", &plane->fb_id));
	CHECK_RESULT(get_prop(fd, props, "CRTC_X", &plane->crtc_x));
	CHECK_RESULT(get_prop(fd, props, "CRTC_Y", &plane->crtc_y));
	CHECK_RESULT(get_prop(fd, props, "CRTC_W", &plane->crtc_w));
	CHECK_RESULT(get_prop(fd, props, "CRTC_H", &plane->crtc_h));
	CHECK_RESULT(get_prop(fd, props, "SRC_X", &plane->src_x));
	CHECK_RESULT(get_prop(fd, props, "SRC_Y", &plane->src_y));
	CHECK_RESULT(get_prop(fd, props, "SRC_W", &plane->src_w));
	CHECK_RESULT(get_prop(fd, props, "SRC_H", &plane->src_h));
	CHECK_RESULT(get_prop(fd, props, "type", &plane->type));
	return 0;
}

int set_connector_props(struct atomictest_connector *conn, drmModeAtomicReqPtr pset)
{
	uint32_t id = conn->connector_id;

	/*
	 * Currently, kernel v4.4 doesn't have CRTC_ID as a property of the connector. It's
	 * required for the modeset to work, so we currently just take it from a plane. Also
	 * setting EDID or DPMS (even w/o modification) makes the kernel return -EINVAL, so
	 * let's keep them unset for now.
	 */
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, conn->crtc_id.pid, conn->crtc_id.value));
	return 0;
}

int set_crtc_props(struct atomictest_crtc *crtc, drmModeAtomicReqPtr pset)
{
	uint32_t id = crtc->crtc_id;
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, crtc->mode_id.pid, crtc->mode_id.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, crtc->active.pid, crtc->active.value));
	return 0;
}

int set_plane_props(struct atomictest_plane *plane, drmModeAtomicReqPtr pset)
{
	uint32_t id = plane->drm_plane.plane_id;
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->crtc_id.pid, plane->crtc_id.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->fb_id.pid, plane->fb_id.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->crtc_x.pid, plane->crtc_x.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->crtc_y.pid, plane->crtc_y.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->crtc_w.pid, plane->crtc_w.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->crtc_h.pid, plane->crtc_h.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->src_x.pid, plane->src_x.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->src_y.pid, plane->src_y.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->src_w.pid, plane->src_w.value));
	CHECK_RESULT(drmModeAtomicAddProperty(pset, id, plane->src_h.pid, plane->src_h.value));
	return 0;
}

static int remove_plane_fb(struct atomictest_context *ctx, struct atomictest_plane *plane)
{
	if (plane->bo && plane->fb_id.value) {
		CHECK_RESULT(drmModeRmFB(ctx->fd, plane->fb_id.value));
		gbm_bo_destroy(plane->bo);
		plane->bo = NULL;
		plane->fb_id.value = 0;
	}

	return 0;
}

static int add_plane_fb(struct atomictest_context *ctx, struct atomictest_plane *plane)
{
	if (plane->format_idx < plane->drm_plane.count_formats) {
		CHECK_RESULT(remove_plane_fb(ctx, plane));
		uint32_t flags = (plane->type.value == DRM_PLANE_TYPE_CURSOR) ? GBM_BO_USE_CURSOR
									      : GBM_BO_USE_SCANOUT;
		/* TODO(gsingh): add create with modifiers option. */
		plane->bo = gbm_bo_create(gbm, plane->crtc_w.value, plane->crtc_h.value,
					  plane->drm_plane.formats[plane->format_idx], flags);

		CHECK(plane->bo);
		plane->fb_id.value = bs_drm_fb_create_gbm(plane->bo);
		CHECK(plane->fb_id.value);
		CHECK_RESULT(set_plane_props(plane, ctx->pset));
	}

	return 0;
}

static int init_plane(struct atomictest_context *ctx, struct atomictest_plane *plane,
		      uint32_t format, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		      uint32_t zpos, uint32_t crtc_id)
{
	int32_t idx = get_format_idx(plane, format);
	if (idx < 0)
		return -EINVAL;

	plane->format_idx = idx;
	plane->crtc_x.value = x;
	plane->crtc_y.value = y;
	plane->crtc_w.value = w;
	plane->crtc_h.value = h;
	plane->src_w.value = plane->crtc_w.value << 16;
	plane->src_h.value = plane->crtc_h.value << 16;
	plane->zpos.value = zpos;
	plane->crtc_id.value = crtc_id;

	CHECK_RESULT(add_plane_fb(ctx, plane));
	return 0;
}

static int disable_plane(struct atomictest_context *ctx, struct atomictest_plane *plane)
{
	plane->format_idx = 0;
	plane->crtc_x.value = 0;
	plane->crtc_y.value = 0;
	plane->crtc_w.value = 0;
	plane->crtc_h.value = 0;
	plane->src_w.value = 0;
	plane->src_h.value = 0;
	plane->zpos.value = 0;
	plane->crtc_id.value = 0;

	CHECK_RESULT(remove_plane_fb(ctx, plane));
	CHECK_RESULT(set_plane_props(plane, ctx->pset));
	return 0;
}

static int move_plane(struct atomictest_context *ctx, struct atomictest_crtc *crtc,
		      struct atomictest_plane *plane, uint32_t dx, uint32_t dy)
{
	if (plane->crtc_x.value < (crtc->width - plane->crtc_w.value) &&
	    plane->crtc_y.value < (crtc->height - plane->crtc_h.value)) {
		plane->crtc_x.value += dx;
		plane->crtc_y.value += dy;
		CHECK_RESULT(set_plane_props(plane, ctx->pset));
		return 0;
	}

	return -1;
}

static int commit(struct atomictest_context *ctx)
{
	int ret;
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(ctx->fd, &fds);

	ret = drmModeAtomicCommit(ctx->fd, ctx->pset,
				  DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	CHECK_RESULT(ret);
	do {
		ret = select(ctx->fd + 1, &fds, NULL, NULL, NULL);
	} while (ret == -1 && errno == EINTR);

	CHECK_RESULT(ret);
	if (FD_ISSET(ctx->fd, &fds))
		drmHandleEvent(ctx->fd, &ctx->drm_event_ctx);

	return 0;
}

static int pageflip(struct atomictest_context *ctx, struct atomictest_plane *plane, uint32_t x,
		    uint32_t y, uint32_t w, uint32_t h, uint32_t zpos, uint32_t crtc_id,
		    uint32_t *formats, uint32_t count_formats)
{
	/* Check if plane support specified formats. */
	for (uint32_t i = 0; i < count_formats; i++)
		CHECK_RESULT(get_format_idx(plane, formats[i]));

	for (uint32_t i = 0; i < count_formats; i++) {
		CHECK_RESULT(init_plane(ctx, plane, formats[i], x, y, w, h, zpos, crtc_id));
		write_to_buffer(ctx->mapper, plane->bo, 0x00FF0000, 0xF800);
		CHECK_RESULT(commit(ctx));
		usleep(1e6);
	}

	return 0;
}

static int check_mode(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	drmModeAtomicSetCursor(ctx->pset, 0);

	for (uint32_t i = 0; i < ctx->num_crtcs; i++) {
		if (&ctx->crtcs[i] != crtc) {
			ctx->crtcs[i].mode_id.value = 0;
			ctx->crtcs[i].active.value = 0;
			set_crtc_props(&ctx->crtcs[i], ctx->pset);
		}
	}

	for (uint32_t i = 0; i < ctx->num_connectors; i++) {
		ctx->connectors[i].crtc_id.value = crtc->crtc_id;
		set_connector_props(&ctx->connectors[i], ctx->pset);
	}

	int ret = -EINVAL;
	int cursor = drmModeAtomicGetCursor(ctx->pset);

	for (uint32_t i = 0; i < ctx->num_modes; i++) {
		struct atomictest_mode *mode = &ctx->modes[i];
		drmModeAtomicSetCursor(ctx->pset, cursor);

		crtc->mode_id.value = mode->id;
		crtc->active.value = 1;
		crtc->width = mode->width;
		crtc->height = mode->height;

		set_crtc_props(crtc, ctx->pset);
		ret = drmModeAtomicCommit(ctx->fd, ctx->pset,
					  DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
					  NULL);
		if (!ret)
			return 0;
	}

	bs_debug_error("[CRTC:%d]: failed to find mode", crtc->crtc_id);
	return ret;
}

static void free_context(struct atomictest_context *ctx)
{
	for (uint32_t i = 0; i < ctx->num_crtcs; i++) {
		uint32_t num_planes = ctx->crtcs[i].num_primary + ctx->crtcs[i].num_cursor +
				      ctx->crtcs[i].num_overlay;

		for (uint32_t j = 0; j < num_planes; j++) {
			remove_plane_fb(ctx, &ctx->crtcs[i].planes[j]);
			free(ctx->crtcs[i].planes[j].drm_plane.formats);
			free(ctx->crtcs[i].planes[j].drm_plane.format_modifiers);
		}

		free(ctx->crtcs[i].planes);
		free(ctx->crtcs[i].overlay_idx);
		free(ctx->crtcs[i].cursor_idx);
		free(ctx->crtcs[i].primary_idx);
	}

	drmModeAtomicFree(ctx->pset);
	free(ctx->modes);
	free(ctx->crtcs);
	free(ctx->connectors);
	bs_mapper_destroy(ctx->mapper);
	free(ctx);
}

static struct atomictest_context *new_context(uint32_t num_connectors, uint32_t num_crtcs,
					      uint32_t num_planes)
{
	struct atomictest_context *ctx = calloc(1, sizeof(*ctx));
	ctx->connectors = calloc(num_connectors, sizeof(*ctx->connectors));
	ctx->crtcs = calloc(num_crtcs, sizeof(*ctx->crtcs));
	for (uint32_t i = 0; i < num_crtcs; i++) {
		ctx->crtcs[i].planes = calloc(num_planes, sizeof(*ctx->crtcs[i].planes));
		ctx->crtcs[i].overlay_idx = calloc(num_planes, sizeof(uint32_t));
		ctx->crtcs[i].primary_idx = calloc(num_planes, sizeof(uint32_t));
		ctx->crtcs[i].cursor_idx = calloc(num_planes, sizeof(uint32_t));
	}

	ctx->num_connectors = num_connectors;
	ctx->num_crtcs = num_crtcs;
	ctx->num_modes = 0;
	ctx->modes = NULL;
	ctx->pset = drmModeAtomicAlloc();
	ctx->drm_event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
	ctx->drm_event_ctx.page_flip_handler = page_flip_handler;

	ctx->mapper = bs_mapper_gem_new();
	if (ctx->mapper == NULL) {
		bs_debug_error("failed to create mapper object");
		free_context(ctx);
		return NULL;
	}

	return ctx;
}

static struct atomictest_context *init_atomictest(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (res == NULL) {
		bs_debug_error("failed to get drm resources");
		return false;
	}

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
	if (plane_res == NULL) {
		bs_debug_error("failed to get plane resources");
		drmModeFreeResources(res);
		return NULL;
	}

	struct atomictest_context *ctx =
	    new_context(res->count_connectors, res->count_crtcs, plane_res->count_planes);
	if (ctx == NULL) {
		bs_debug_error("failed to allocate atomic context");
		drmModeFreePlaneResources(plane_res);
		drmModeFreeResources(res);
		return NULL;
	}

	ctx->fd = fd;
	drmModeObjectPropertiesPtr props = NULL;

	for (uint32_t conn_index = 0; conn_index < res->count_connectors; conn_index++) {
		uint32_t conn_id = res->connectors[conn_index];
		ctx->connectors[conn_index].connector_id = conn_id;
		props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
		get_connector_props(fd, &ctx->connectors[conn_index], props);

		drmModeConnector *connector = drmModeGetConnector(fd, conn_id);
		for (uint32_t mode_index = 0; mode_index < connector->count_modes; mode_index++) {
			ctx->modes = realloc(ctx->modes, (ctx->num_modes + 1) * sizeof(*ctx->modes));
			drmModeCreatePropertyBlob(fd, &connector->modes[mode_index],
						  sizeof(drmModeModeInfo),
						  &ctx->modes[ctx->num_modes].id);
			ctx->modes[ctx->num_modes].width = connector->modes[mode_index].hdisplay;
			ctx->modes[ctx->num_modes].height = connector->modes[mode_index].vdisplay;
			ctx->num_modes++;
		}

		drmModeFreeConnector(connector);
		drmModeFreeObjectProperties(props);
		props = NULL;
	}

	uint32_t crtc_index;
	for (crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++) {
		ctx->crtcs[crtc_index].crtc_id = res->crtcs[crtc_index];
		props =
		    drmModeObjectGetProperties(fd, res->crtcs[crtc_index], DRM_MODE_OBJECT_CRTC);
		get_crtc_props(fd, &ctx->crtcs[crtc_index], props);

		drmModeFreeObjectProperties(props);
		props = NULL;
	}

	uint32_t overlay_idx, primary_idx, cursor_idx, idx;

	for (uint32_t plane_index = 0; plane_index < plane_res->count_planes; plane_index++) {
		drmModePlane *plane = drmModeGetPlane2(fd, plane_res->planes[plane_index]);
		if (plane == NULL) {
			bs_debug_error("failed to get plane id %u", plane_res->planes[plane_index]);
			continue;
		}

		uint32_t crtc_mask = 0;

		drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
		    fd, plane_res->planes[plane_index], DRM_MODE_OBJECT_PLANE);

		for (crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++) {
			crtc_mask = (1 << crtc_index);
			if (plane->possible_crtcs & crtc_mask) {
				struct atomictest_crtc *crtc = &ctx->crtcs[crtc_index];
				cursor_idx = crtc->num_cursor;
				primary_idx = crtc->num_primary;
				overlay_idx = crtc->num_overlay;
				idx = cursor_idx + primary_idx + overlay_idx;
				copy_drm_plane(&crtc->planes[idx].drm_plane, plane);
				get_plane_props(fd, &crtc->planes[idx], props);
				switch (crtc->planes[idx].type.value) {
					case DRM_PLANE_TYPE_OVERLAY:
						crtc->overlay_idx[overlay_idx] = idx;
						crtc->num_overlay++;
						break;
					case DRM_PLANE_TYPE_PRIMARY:
						crtc->primary_idx[primary_idx] = idx;
						crtc->num_primary++;
						break;
					case DRM_PLANE_TYPE_CURSOR:
						crtc->cursor_idx[cursor_idx] = idx;
						crtc->num_cursor++;
						break;
					default:
						bs_debug_error("invalid plane type returned");
						return NULL;
				}
			}
		}

		drmModeFreePlane(plane);
		drmModeFreeObjectProperties(props);
		props = NULL;
	}

	/* HACK: Set connector CRTC pid to plane CRTC pid. */
	for (uint32_t i = 0; i < ctx->num_connectors; i++)
		ctx->connectors[i].crtc_id.pid = ctx->crtcs[0].planes[0].crtc_id.pid;

	drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(res);
	return ctx;
}

static int run_atomictest(const struct atomictest *test)
{
	int ret = 0;
	int fd = bs_drm_open_main_display();
	CHECK_RESULT(fd);

	gbm = gbm_create_device(fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm device");
		ret = 1;
		goto destroy_fd;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		bs_debug_error("failed to enable DRM_CLIENT_CAP_UNIVERSAL_PLANES");
		ret = 1;
		goto destroy_gbm_device;
	}

	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		bs_debug_error("failed to enable DRM_CLIENT_CAP_ATOMIC");
		ret = 1;
		goto destroy_gbm_device;
	}

	struct atomictest_context *ctx = init_atomictest(fd);
	if (!ctx) {
		bs_debug_error("initializing atomictest failed.");
		ret = 1;
		goto destroy_gbm_device;
	}

	struct atomictest_crtc *crtc;
	for (uint32_t crtc_index = 0; crtc_index < ctx->num_crtcs; crtc_index++) {
		crtc = &ctx->crtcs[crtc_index];
		if (!check_mode(ctx, crtc))
			ret |= test->run_test(ctx, crtc);
		else
			ret |= 1;
	}

	free_context(ctx);

destroy_gbm_device:
	gbm_device_destroy(gbm);
destroy_fd:
	close(fd);

	return ret;
}

static int test_multiple_planes(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	struct atomictest_plane *primary, *overlay, *cursor;
	for (uint32_t i = 0; i < crtc->num_primary; i++) {
		bool has_video = false;
		uint32_t x, y;
		for (uint32_t j = 0; j < crtc->num_overlay; j++) {
			overlay = get_plane(crtc, j, DRM_PLANE_TYPE_OVERLAY);
			x = crtc->width >> (j + 2);
			y = crtc->height >> (j + 2);
			bool added_video = false;
			if (!has_video) {
				uint32_t k = 0;
				for (k = 0; k < BS_ARRAY_LEN(yuv_formats); k++) {
					if (!init_plane(ctx, overlay, yuv_formats[k], x, y, x, y, j,
							crtc->crtc_id)) {
						has_video = true;
						added_video = true;
						break;
					}
				}

				if (added_video) {
					const struct bs_draw_format *draw_format =
					    bs_get_draw_format(yuv_formats[k]);
					CHECK(draw_format);
					CHECK(
					    bs_draw_stripe(ctx->mapper, overlay->bo, draw_format));
				}
			}

			if (!added_video) {
				added_video = true;
				CHECK_RESULT(init_plane(ctx, overlay, DRM_FORMAT_XRGB8888, x, y, x,
							y, i, crtc->crtc_id));
				write_to_buffer(ctx->mapper, overlay->bo, 0x00FF0000, 0);
			}
		}

		for (uint32_t j = 0; j < crtc->num_cursor; j++) {
			x = crtc->width >> (j + 2);
			y = crtc->height >> (j + 2);
			cursor = get_plane(crtc, j, DRM_PLANE_TYPE_CURSOR);
			CHECK_RESULT(init_plane(ctx, cursor, DRM_FORMAT_XRGB8888, x, y, CURSOR_SIZE,
						CURSOR_SIZE, crtc->num_overlay + j, crtc->crtc_id));
			draw_cursor(ctx->mapper, cursor->bo);
		}

		primary = get_plane(crtc, i, DRM_PLANE_TYPE_PRIMARY);
		CHECK_RESULT(init_plane(ctx, primary, DRM_FORMAT_XRGB8888, 0, 0, crtc->width,
					crtc->height, 0, crtc->crtc_id));
		write_to_buffer(ctx->mapper, primary->bo, 0x00000FF, 0);

		uint32_t num_planes = crtc->num_primary + crtc->num_cursor + crtc->num_overlay;
		int done = 0;
		struct atomictest_plane *plane;
		while (!done) {
			done = 1;
			for (uint32_t j = 0; j < num_planes; j++) {
				plane = &crtc->planes[j];
				if (plane->type.value != DRM_PLANE_TYPE_PRIMARY)
					done &= move_plane(ctx, crtc, plane, 20, 20);
			}

			CHECK_RESULT(commit(ctx));
			usleep(1e6 / 60);
		}

		CHECK_RESULT(commit(ctx));
		usleep(1e6);

		/* Disable primary plane and verify overlays show up. */
		CHECK_RESULT(disable_plane(ctx, primary));
		CHECK_RESULT(commit(ctx));
		usleep(1e6);
	}

	return 0;
}

static int test_video_overlay(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	struct atomictest_plane *overlay;
	for (uint32_t i = 0; i < crtc->num_overlay; i++) {
		overlay = get_plane(crtc, i, DRM_PLANE_TYPE_OVERLAY);
		for (uint32_t j = 0; j < BS_ARRAY_LEN(yuv_formats); j++) {
			if (init_plane(ctx, overlay, yuv_formats[j], 0, 0, 800, 800, 0,
				       crtc->crtc_id))
				continue;

			const struct bs_draw_format *draw_format =
			    bs_get_draw_format(yuv_formats[j]);
			CHECK(draw_format);
			CHECK(bs_draw_stripe(ctx->mapper, overlay->bo, draw_format));
			while (!move_plane(ctx, crtc, overlay, 20, 20)) {
				CHECK_RESULT(commit(ctx));
				usleep(1e6 / 60);
			}
		}
	}

	return 0;
}

static int test_fullscreen_video(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	struct atomictest_plane *primary;
	for (uint32_t i = 0; i < crtc->num_primary; i++) {
		primary = get_plane(crtc, i, DRM_PLANE_TYPE_PRIMARY);
		for (uint32_t j = 0; j < BS_ARRAY_LEN(yuv_formats); j++) {
			if (init_plane(ctx, primary, yuv_formats[j], 0, 0, crtc->width,
				       crtc->height, 0, crtc->crtc_id))
				continue;
			const struct bs_draw_format *draw_format =
			    bs_get_draw_format(yuv_formats[j]);
			CHECK(draw_format);

			CHECK(bs_draw_stripe(ctx->mapper, primary->bo, draw_format));
			CHECK_RESULT(commit(ctx));
			usleep(1e6);
		}
	}

	return 0;
}

static int test_disable_primary(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	int cursor;
	struct atomictest_plane *primary, *overlay;
	for (uint32_t i = 0; i < crtc->num_primary; i++) {
		for (uint32_t j = 0; j < crtc->num_overlay; j++) {
			overlay = get_plane(crtc, j, DRM_PLANE_TYPE_OVERLAY);
			uint32_t x = crtc->width >> (j + 2);
			uint32_t y = crtc->height >> (j + 2);
			CHECK_RESULT(init_plane(ctx, overlay, DRM_FORMAT_XRGB8888, x, y, x, y, i,
						crtc->crtc_id));
			write_to_buffer(ctx->mapper, overlay->bo, 0x00FF0000, 0);
		}

		cursor = drmModeAtomicGetCursor(ctx->pset);
		primary = get_plane(crtc, i, DRM_PLANE_TYPE_PRIMARY);
		CHECK_RESULT(init_plane(ctx, primary, DRM_FORMAT_XRGB8888, 0, 0, crtc->width,
					crtc->height, 0, crtc->crtc_id));
		write_to_buffer(ctx->mapper, primary->bo, 0x00000FF, 0);
		CHECK_RESULT(commit(ctx));
		usleep(1e6);

		/* Disable primary plane. */
		disable_plane(ctx, primary);
		CHECK_RESULT(commit(ctx));
		usleep(1e6);
		drmModeAtomicSetCursor(ctx->pset, cursor);
	}

	return 0;
}

static int test_overlay_pageflip(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	struct atomictest_plane *overlay;
	uint32_t formats[3] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_RGB565 };
	for (uint32_t i = 0; i < crtc->num_overlay; i++) {
		overlay = get_plane(crtc, i, DRM_PLANE_TYPE_OVERLAY);
		uint32_t x = crtc->width >> (i + 1);
		uint32_t y = crtc->height >> (i + 1);

		CHECK_RESULT(pageflip(ctx, overlay, x, y, x, y, i, crtc->crtc_id, formats,
				      BS_ARRAY_LEN(formats)));
	}

	return 0;
}

static int test_primary_pageflip(struct atomictest_context *ctx, struct atomictest_crtc *crtc)
{
	int cursor;
	struct atomictest_plane *primary;
	uint32_t formats[3] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_RGB565 };
	for (uint32_t i = 0; i < crtc->num_primary; i++) {
		primary = get_plane(crtc, i, DRM_PLANE_TYPE_PRIMARY);
		cursor = drmModeAtomicGetCursor(ctx->pset);
		CHECK_RESULT(pageflip(ctx, primary, 0, 0, crtc->width, crtc->height, 0,
				      crtc->crtc_id, formats, BS_ARRAY_LEN(formats)));

		drmModeAtomicSetCursor(ctx->pset, cursor);
	}

	return 0;
}

static const struct atomictest tests[] = {
	{ "disable_primary", test_disable_primary },
	{ "fullscreen_video", test_fullscreen_video },
	{ "multiple_planes", test_multiple_planes },
	{ "overlay_pageflip", test_overlay_pageflip },
	{ "primary_pageflip", test_primary_pageflip },
	{ "video_overlay", test_video_overlay },
};

static void print_help(const char *argv0)
{
	printf("usage: %s <test_name>\n\n", argv0);
	printf("A valid name test is one the following:\n");
	for (uint32_t i = 0; i < BS_ARRAY_LEN(tests); i++)
		printf("%s\n", tests[i].name);
}

int main(int argc, char **argv)
{
	if (argc == 2) {
		char *name = argv[1];
		for (uint32_t i = 0; i < BS_ARRAY_LEN(tests); i++) {
			if (strcmp(tests[i].name, name))
				continue;

			int ret = run_atomictest(&tests[i]);
			if (ret) {
				printf("[  FAILED  ] atomictest.%s\n", name);
				return -1;
			} else {
				printf("[  PASSED  ] atomictest.%s\n", name);
				return 0;
			}
		}
	}

	print_help(argv[0]);

	return -1;
}
