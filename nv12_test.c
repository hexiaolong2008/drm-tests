/*
 * Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "bo.h"
#include "dev.h"

static int
is_internal(uint32_t connector_type)
{
	return connector_type == DRM_MODE_CONNECTOR_LVDS ||
	       connector_type == DRM_MODE_CONNECTOR_eDP ||
	       connector_type == DRM_MODE_CONNECTOR_DSI;
}

int
find_crtc_encoder_connector(int fd, drmModeRes *resources, uint32_t *crtc_id,
			    uint32_t *encoder_id, uint32_t *connector_id)
{
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int c, e;
	for (c = 0; c < resources->count_connectors; c++) {
		printf("Trying connector %d\n", resources->connectors[c]);
		connector = drmModeGetConnector(fd, resources->connectors[c]);
		if (!connector)
			continue;
		printf("connected %d type %d count_modes %d\n", connector->connection, connector->count_modes, connector->connector_type);
		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->count_modes > 0 &&
		    is_internal(connector->connector_type)) {
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	if (!connector)
		return 0;

	for (e = 0; e < connector->count_encoders; e++) {
		printf("trying encoder %d\n", connector->encoders[e]);
		encoder = drmModeGetEncoder(fd, connector->encoders[e]);
		if (!encoder)
			continue;

		for (c = 0; c < resources->count_crtcs; c++) {
			if (encoder->possible_crtcs & (1u << c)) {
				printf("trying crtc [%d] = %d\n", c, resources->crtcs[c]);
				*crtc_id = resources->crtcs[c];
				*encoder_id = connector->encoders[e];
				*connector_id = connector->connector_id;
				drmModeFreeConnector(connector);
				drmModeFreeEncoder(encoder);
				return 1;
			}
		}
		drmModeFreeEncoder(encoder);
	}
	drmModeFreeConnector(connector);
	return 0;
}

int
find_best_mode(int fd, uint32_t connector_id, drmModeModeInfoPtr mode)
{
	int m;
	int found = 0;
	drmModeConnector *connector;

	connector = drmModeGetConnector(fd, connector_id);
	if (!connector)
		return 0;

	for (m = 0; m < connector->count_modes && !found; m++) {
		if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
			*mode = connector->modes[m];
			found = 1;
		}
	}

	if (!found) {
		*mode = connector->modes[0];
		found = 1;
	}
	drmModeFreeConnector(connector);
	return found;
}

static void
draw_pattern(struct sp_bo *bo)
{
	uint32_t stripw = bo->width / 256;
	uint32_t striph = bo->height / 4;
	uint32_t x;

	fill_bo(bo, 0, 0, 0, 0);
	for (x = 0; x < 256; x++) {
		draw_rect(bo, x * stripw, 0, stripw, striph, 0, x, x, x);
		draw_rect(bo, x * stripw, striph, stripw, striph, 0, x, 0, 0);
		draw_rect(bo, x * stripw, striph * 2, stripw, striph, 0, 0, x, 0);
		draw_rect(bo, x * stripw, striph * 3, stripw, striph, 0, 0, 0, x);
	}
}

int main(int argc, char **argv)
{
	drmModeRes *resources;
	struct sp_dev *dev;

	dev = create_sp_dev();
	if (!dev) {
		fprintf(stderr, "Creating DRM device failed\n");
		return 1;
	}

	resources = drmModeGetResources(dev->fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
		return 1;
	}

	{
		int ret;
		drmModeModeInfo mode;
		uint32_t crtc_id = 0, encoder_id = 0, connector_id = 0;
		struct sp_bo *bo;

		if (!find_crtc_encoder_connector(dev->fd, resources, &crtc_id,
					     &encoder_id, &connector_id)) {
			fprintf(stderr,
				"Could not find crtc, connector and encoder\n");
			return -1;
		}
		printf("Using CRTC:%u ENCODER:%u CONNECTOR:%u\n",
		       crtc_id, encoder_id, connector_id);

		if (!find_best_mode(dev->fd, connector_id, &mode)) {
			fprintf(stderr, "Could not find mode for CRTC %d\n",
			        crtc_id);
			return -1;
		}
		printf("Using mode %s\n", mode.name);

		printf("Creating buffer %ux%u\n", mode.hdisplay, mode.vdisplay);
		bo = create_sp_bo(dev, mode.hdisplay, mode.vdisplay + mode.vdisplay/2, 8, 8,
				  DRM_FORMAT_NV12, 0);

		draw_pattern(bo);

		ret = drmModeSetCrtc(dev->fd, crtc_id, bo->fb_id, 0, 0,
				     &connector_id, 1, &mode);
		if (ret < 0) {
			fprintf(stderr, "Could not set mode on CRTC %d %s\n",
				crtc_id, strerror(errno));
			return 1;
		}

		sleep(5);

		ret = drmModeSetCrtc(dev->fd, crtc_id, 0, 0, 0, NULL, 0,
				     NULL);
		if (ret < 0) {
			fprintf(stderr, "Could not disable CRTC %d %s\n",
				crtc_id, strerror(errno));
		}
		free_sp_bo(bo);
	}

	drmModeFreeResources(resources);


	return 0;
}
