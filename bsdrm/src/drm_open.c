/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

static const uint32_t kRankSkip = UINT32_MAX;

// A return value of true causes enumeration to end immediately. fd is always
// closed after the callback.
typedef bool (*open_enumerate)(void *user, int fd);

// A return value of true causes the filter to return the given fd.
typedef bool (*open_filter)(int fd);

// The fd with the lowest (magnitude) rank is returned. A fd with rank UINT32_MAX is skipped. A fd
// with rank 0 ends the enumeration early and is returned. On a tie, the fd returned will be
// arbitrarily chosen from the set of lowest rank fds.
typedef uint32_t (*open_rank)(int fd);

// Suppresses warnings for our usage of asprintf with one of the parameters.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static void bs_drm_enumerate(const char *format, unsigned start, unsigned end, open_enumerate body,
			     void *user)
{
	assert(end >= start);
	for (unsigned dev_index = start; dev_index < end; dev_index++) {
		char *file_path = NULL;
		int ret = asprintf(&file_path, format, dev_index);
		if (ret == -1)
			return;
		assert(file_path);

		int fd = open(file_path, O_RDWR);
		free(file_path);
		if (fd < 0)
			return;

		bool end = body(user, fd);
		close(fd);

		if (end)
			return;
	}
}
#pragma GCC diagnostic pop

struct bs_drm_open_filtered_user {
	open_filter filter;
	int fd;
};

static bool bs_drm_open_filtered_body(void *user, int fd)
{
	struct bs_drm_open_filtered_user *data = (struct bs_drm_open_filtered_user *)user;
	if (data->filter(fd)) {
		data->fd = dup(fd);
		return true;
	}

	return false;
}

static int bs_drm_open_filtered(const char *format, unsigned start, unsigned end,
				open_filter filter)
{
	struct bs_drm_open_filtered_user data = {filter, -1};
	bs_drm_enumerate(format, start, end, bs_drm_open_filtered_body, &data);
	return data.fd;
}

struct bs_drm_open_ranked_user {
	open_rank rank;
	uint32_t rank_index;
	int fd;
};

static bool bs_drm_open_ranked_body(void *user, int fd)
{
	struct bs_drm_open_ranked_user *data = (struct bs_drm_open_ranked_user *)user;
	uint32_t rank_index = data->rank(fd);

	if (data->rank_index > rank_index) {
		data->rank_index = rank_index;
		if (data->fd >= 0)
			close(data->fd);
		data->fd = dup(fd);
	}

	return rank_index == 0;
}

static int bs_drm_open_ranked(const char *format, unsigned start, unsigned end, open_rank rank)
{
	struct bs_drm_open_ranked_user data = {rank, UINT32_MAX, -1};
	bs_drm_enumerate(format, start, end, bs_drm_open_ranked_body, &data);
	return data.fd;
}

static bool display_filter(int fd)
{
	bool has_connection = false;
	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
		return false;

	if (res->count_crtcs == 0)
		goto out;

	for (int connector_index = 0; connector_index < res->count_connectors; connector_index++) {
		drmModeConnector *connector =
		    drmModeGetConnector(fd, res->connectors[connector_index]);
		if (connector == NULL)
			continue;

		has_connection =
		    connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0;
		drmModeFreeConnector(connector);
		if (has_connection)
			break;
	}

out:
	drmModeFreeResources(res);
	return has_connection;
}

int bs_drm_open_for_display()
{
	return bs_drm_open_filtered("/dev/dri/card%u", 0, DRM_MAX_MINOR, display_filter);
}

static bool connector_has_crtc(int fd, drmModeRes *res, drmModeConnector *connector)
{
	for (int encoder_index = 0; encoder_index < connector->count_encoders; encoder_index++) {
		drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoders[encoder_index]);
		if (encoder == NULL)
			continue;

		uint32_t possible_crtcs = encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);

		for (int crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++)
			if ((possible_crtcs & (1 << crtc_index)) != 0)
				return true;
	}

	return false;
}

static uint32_t display_rank_connetor_type(uint32_t connector_type)
{
	switch (connector_type) {
		case DRM_MODE_CONNECTOR_LVDS:
			return 0x01;
		case DRM_MODE_CONNECTOR_eDP:
			return 0x02;
		case DRM_MODE_CONNECTOR_DSI:
			return 0x03;
	}
	return 0xFF;
}

static uint32_t display_rank(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
		return kRankSkip;

	uint32_t best_rank = kRankSkip;
	if (res->count_crtcs == 0)
		goto out;

	for (int connector_index = 0; connector_index < res->count_connectors; connector_index++) {
		drmModeConnector *connector =
		    drmModeGetConnector(fd, res->connectors[connector_index]);
		if (connector == NULL)
			continue;

		bool has_connection = connector->connection == DRM_MODE_CONNECTED &&
				      connector->count_modes > 0 &&
				      connector_has_crtc(fd, res, connector);
		drmModeFreeConnector(connector);
		if (!has_connection)
			continue;

		uint32_t rank = display_rank_connetor_type(connector->connector_type);
		if (best_rank > rank)
			best_rank = rank;
	}

out:
	drmModeFreeResources(res);
	return best_rank;
}

int bs_drm_open_main_display()
{
	return bs_drm_open_ranked("/dev/dri/card%u", 0, DRM_MAX_MINOR, display_rank);
}

static bool vgem_filter(int fd)
{
	drmVersion *version = drmGetVersion(fd);
	if (!version)
		return false;

	bool is_vgem = (strncmp("vgem", version->name, version->name_len) == 0);
	drmFreeVersion(version);
	return is_vgem;
}

int bs_drm_open_vgem()
{
	return bs_drm_open_filtered("/dev/dri/card%u", 0, DRM_MAX_MINOR, vgem_filter);
}
