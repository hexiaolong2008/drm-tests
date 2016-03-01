/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

struct pipe_internal {
	int connector_index;
	int next_connector_index;
	int encoder_index;
	int next_encoder_index;
	int crtc_index;
	int next_crtc_index;
};

struct pipe_ctx {
	int fd;
	drmModeRes *res;
};

static bool pipe_piece_connector(void *c, void *p)
{
	struct pipe_ctx *ctx = c;
	struct pipe_internal *pipe = p;
	int fd = ctx->fd;
	drmModeRes *res = ctx->res;

	bool use_connector = false;
	int connector_index;
	for (connector_index = pipe->next_connector_index; connector_index < res->count_connectors;
	     connector_index++) {
		drmModeConnector *connector =
		    drmModeGetConnector(fd, res->connectors[connector_index]);
		if (connector == NULL)
			continue;

		use_connector =
		    connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0;

		drmModeFreeConnector(connector);

		if (use_connector) {
			pipe->connector_index = connector_index;
			break;
		}
	}

	pipe->next_connector_index = connector_index + 1;
	return use_connector;
}

static bool pipe_piece_encoder(void *c, void *p)
{
	struct pipe_ctx *ctx = c;
	struct pipe_internal *pipe = p;
	int fd = ctx->fd;
	drmModeRes *res = ctx->res;

	drmModeConnector *connector =
	    drmModeGetConnector(fd, res->connectors[pipe->connector_index]);
	if (connector == NULL)
		return false;

	int encoder_index = 0;
	for (encoder_index = pipe->next_encoder_index; encoder_index < connector->count_encoders;
	     encoder_index++) {
		drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoders[encoder_index]);
		if (encoder == NULL)
			continue;

		drmModeFreeEncoder(encoder);

		break;
	}

	drmModeFreeConnector(connector);
	pipe->next_encoder_index = encoder_index + 1;
	return encoder_index < connector->count_encoders;
}

static bool pipe_piece_crtc(void *c, void *p)
{
	struct pipe_ctx *ctx = c;
	struct pipe_internal *pipe = p;
	int fd = ctx->fd;
	drmModeRes *res = ctx->res;

	drmModeEncoder *encoder = drmModeGetEncoder(fd, res->encoders[pipe->encoder_index]);
	if (encoder == NULL)
		return false;

	uint32_t possible_crtcs = encoder->possible_crtcs;
	drmModeFreeEncoder(encoder);

	bool use_crtc = false;
	int crtc_index;
	for (crtc_index = pipe->next_crtc_index; crtc_index < res->count_crtcs; crtc_index++) {
		use_crtc = (possible_crtcs & (1 << crtc_index));
		if (use_crtc) {
			pipe->crtc_index = crtc_index;
			break;
		}
	}

	pipe->next_crtc_index = crtc_index;
	return use_crtc;
}

bool bs_drm_pipe_make(int fd, struct bs_drm_pipe *pipe)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (res == NULL)
		return false;

	struct pipe_ctx ctx = { fd, res };
	struct pipe_internal pipe_internal = { 0 };
	bs_make_pipe_piece pieces[] = { pipe_piece_connector, pipe_piece_encoder, pipe_piece_crtc };
	bool success = bs_pipe_make(&ctx, pieces, sizeof(pieces) / sizeof(pieces[0]),
				    &pipe_internal, sizeof(struct pipe_internal));
	if (!success)
		goto out;

	pipe->connector_id = res->connectors[pipe_internal.connector_index];
	pipe->encoder_id = res->encoders[pipe_internal.encoder_index];
	pipe->crtc_id = res->crtcs[pipe_internal.crtc_index];

out:
	drmModeFreeResources(res);
	return success;
}
