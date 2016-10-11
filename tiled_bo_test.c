/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

struct offscreen_data {
	struct gbm_bo *bo;
	GLuint tex;
	EGLImageKHR image;
};

// clang-format off
static const GLfloat vertices[] = {
	// x       y     u     v
	-0.25f, -0.25f, 0.0f, 0.0f, // Bottom left
	-0.25f,  0.25f, 0.0f, 1.0f, // Top left
	 0.25f,  0.25f, 1.0f, 1.0f, // Top right
	 0.25f, -0.25f, 1.0f, 0.0f, // Bottom Right
};

static const int binding_xy = 0;
static const int binding_uv = 1;

static const GLubyte indices[] = {
	0, 1, 2,
	0, 2, 3
};

// clang-format on

static const GLchar *vert =
    "attribute vec2 xy;\n"
    "attribute vec2 uv;\n"
    "varying vec2 tex_coordinate;\n"
    "void main() {\n"
    "    gl_Position = vec4(xy, 0, 1);\n"
    "    tex_coordinate = uv;\n"
    "}\n";

static const GLchar *frag =
    "precision mediump float;\n"
    "uniform sampler2D ellipse;\n"
    "varying vec2 tex_coordinate;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(ellipse, tex_coordinate);\n"
    "}\n";

static uint32_t compute_color(int x, int y, int w, int h)
{
	uint32_t pixel = 0x00000000;
	float xratio = (x - w / 2) / ((float)(w / 2));
	float yratio = (y - h / 2) / ((float)(h / 2));

	// If a point is on or inside an ellipse, num <= 1.
	float num = xratio * xratio + yratio * yratio;
	uint32_t g = 255 * num;

	if (g < 256)
		pixel = 0x00FF0000 | (g << 8);

	return pixel;
}

static bool draw_ellipse(struct gbm_bo *bo)
{
	void *map_data;
	uint32_t stride;
	uint32_t w = gbm_bo_get_width(bo);
	uint32_t h = gbm_bo_get_height(bo);

	void *addr = gbm_bo_map(bo, 0, 0, w, h, 0, &stride, &map_data, 0);
	if (addr == MAP_FAILED) {
		bs_debug_error("failed to mmap gbm bo");
		return false;
	}

	uint32_t *pixel = (uint32_t *)addr;
	uint32_t pixel_size = sizeof(*pixel);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			pixel[y * (stride / pixel_size) + x] = compute_color(x, y, w, h);
		}
	}

	gbm_bo_unmap(bo, map_data);

	return true;
}

static struct offscreen_data *add_offscreen_texture(struct gbm_device *gbm, struct bs_egl *egl,
						    uint32_t width, uint32_t height)
{
	struct offscreen_data *data = calloc(1, sizeof(struct offscreen_data));

	data->bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
	if (!data->bo) {
		bs_debug_error("failed to allocate offscreen buffer object");
		goto free_offscreen_data;
	}

	if (!draw_ellipse(data->bo)) {
		bs_debug_error("failed to draw ellipse");
		goto free_offscreen_bo;
	}

	data->image = bs_egl_image_create_gbm(egl, data->bo);
	if (data->image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to create offscreen egl image");
		goto free_offscreen_bo;
	}

	glActiveTexture(GL_TEXTURE1);
	glGenTextures(1, &data->tex);
	glBindTexture(GL_TEXTURE_2D, data->tex);

	if (!bs_egl_target_texture2D(egl, data->image)) {
		bs_debug_error("failed to import egl image as texture");
		goto free_offscreen_bo;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	return data;

free_offscreen_bo:
	gbm_bo_destroy(data->bo);
free_offscreen_data:
	free(data);
	return NULL;
}

static int draw_textured_quad(struct bs_egl_fb *fb, uint32_t width, uint32_t height)
{
	int ret = 0;
	uint32_t center_pixel;
	struct bs_gl_program_create_binding bindings[] = {
		{ binding_xy, "xy" }, { binding_uv, "uv" }, { 2, NULL },
	};

	GLuint program = bs_gl_program_create_vert_frag_bind(vert, frag, bindings);
	if (!program) {
		bs_debug_error("failed to compile test case shader program");
		return 1;
	}

	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glBindFramebuffer(GL_FRAMEBUFFER, bs_egl_fb_name(fb));
	glViewport(0, 0, (GLint)width, (GLint)height);

	glClearColor(1.0f, 1.0, 1.0, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);

	glUniform1i(glGetUniformLocation(program, "ellipse"), 1);
	glEnableVertexAttribArray(binding_xy);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);

	glEnableVertexAttribArray(binding_uv);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			      (void *)(2 * sizeof(GLfloat)));

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);

	glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
		     (GLvoid *)&center_pixel);

	if (center_pixel != 0xFF0000FF) {
		bs_debug_error("Incorrect RGBA pixel value: %04x", center_pixel);
		ret = 1;
	}

	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteProgram(program);

	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	uint32_t width;
	uint32_t height;
	int display_fd = bs_drm_open_main_display();
	if (display_fd < 0) {
		bs_debug_error("failed to open card for display");
		ret = 1;
		goto out;
	}

	struct gbm_device *gbm = gbm_create_device(display_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm device");
		ret = 1;
		goto destroy_display_fd;
	}

	struct bs_drm_pipe pipe = { 0 };
	if (!bs_drm_pipe_make(display_fd, &pipe)) {
		bs_debug_error("failed to make pipe");
		ret = 1;
		goto destroy_gbm_device;
	}

	drmModeConnector *connector = drmModeGetConnector(display_fd, pipe.connector_id);
	drmModeModeInfo *mode = &connector->modes[0];
	width = mode->hdisplay;
	height = mode->vdisplay;

	struct gbm_bo *scanout_bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_XRGB8888,
						  GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);

	uint32_t fb = bs_drm_fb_create_gbm(scanout_bo);
	if (!fb) {
		bs_debug_error("failed to create framebuffer from buffer object");
		ret = 1;
		goto destroy_scanout_bo;
	}

	struct bs_egl *egl = bs_egl_new();
	if (!bs_egl_setup(egl)) {
		bs_debug_error("failed to setup egl context");
		ret = 1;
		goto destroy_fb;
	}

	EGLImageKHR image = bs_egl_image_create_gbm(egl, scanout_bo);
	if (image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to make image from buffer object");
		ret = 1;
		goto destroy_egl;
	}

	struct bs_egl_fb *egl_fb = bs_egl_fb_new(egl, image);
	if (!fb) {
		bs_debug_error("failed to make framebuffer from image");
		ret = 1;
		goto destroy_scanout_egl_image;
	}

	struct offscreen_data *data = add_offscreen_texture(gbm, egl, width / 4, height / 4);
	if (!data) {
		bs_debug_error("failed to create offscreen texture");
		ret = 1;
		goto destroy_egl_fb;
	}

	ret = draw_textured_quad(egl_fb, width, height);
	if (ret) {
		bs_debug_error("unable to draw texture correctly");
		goto destroy_offscreen_data;
	}

	ret = drmModeSetCrtc(display_fd, pipe.crtc_id, fb, 0 /* x */, 0 /* y */, &pipe.connector_id,
			     1 /* connector count */, mode);
	if (ret)
		bs_debug_error("failed to set crtc: %d", ret);

	usleep(2000000);

destroy_offscreen_data:
	glDeleteTextures(1, &data->tex);
	bs_egl_image_destroy(egl, &data->image);
	gbm_bo_destroy(data->bo);
destroy_egl_fb:
	bs_egl_fb_destroy(&egl_fb);
destroy_scanout_egl_image:
	bs_egl_image_destroy(egl, &image);
destroy_egl:
	bs_egl_destroy(&egl);
destroy_fb:
	drmModeRmFB(display_fd, fb);
destroy_scanout_bo:
	gbm_bo_destroy(scanout_bo);
destroy_gbm_device:
	gbm_device_destroy(gbm);
destroy_display_fd:
	close(display_fd);
out:
	return ret;
}
