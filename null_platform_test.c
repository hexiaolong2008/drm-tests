/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

GLuint shader_create(GLenum type, const GLchar *src)
{
	GLuint shader = glCreateShader(type);
	if (!shader) {
		bs_debug_error("failed to create shader");
		return 0;
	}

	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		GLint log_len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
		char *shader_log = calloc(log_len, sizeof(char));
		assert(shader_log);
		glGetShaderInfoLog(shader, log_len, NULL, shader_log);
		bs_debug_error("failed to compile shader: %s", shader_log);
		free(shader_log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

GLuint solid_shader_create()
{
	const GLchar *vertex_shader_str =
	    "attribute vec4 vPosition;\n"
	    "attribute vec4 vColor;\n"
	    "varying vec4 vFillColor;\n"
	    "void main() {\n"
	    "  gl_Position = vPosition;\n"
	    "  vFillColor = vColor;\n"
	    "}\n";

	const GLchar *fragment_shader_str =
	    "precision mediump float;\n"
	    "varying vec4 vFillColor;\n"
	    "void main() {\n"
	    "  gl_FragColor = vFillColor;\n"
	    "}\n";

	GLuint program = glCreateProgram();
	if (!program) {
		bs_debug_error("failed to create program");
		return 0;
	}

	GLuint vert_shader = shader_create(GL_VERTEX_SHADER, vertex_shader_str);
	if (!vert_shader) {
		bs_debug_error("failed to create vertex shader");
		glDeleteProgram(program);
		return 0;
	}

	GLuint frag_shader = shader_create(GL_FRAGMENT_SHADER, fragment_shader_str);
	if (!frag_shader) {
		bs_debug_error("failed to create fragment shader");
		glDeleteShader(vert_shader);
		glDeleteProgram(program);
		return 0;
	}

	glAttachShader(program, vert_shader);
	glAttachShader(program, frag_shader);
	glBindAttribLocation(program, 0, "vPosition");
	glBindAttribLocation(program, 1, "vColor");
	glLinkProgram(program);
	glDetachShader(program, vert_shader);
	glDetachShader(program, frag_shader);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);

	GLint status;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		GLint log_len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
		char *program_log = calloc(log_len, sizeof(char));
		assert(program_log);
		glGetProgramInfoLog(program, log_len, NULL, program_log);
		bs_debug_error("failed to link program: %s", program_log);
		free(program_log);
		glDeleteProgram(program);
		return 0;
	}

	return program;
}

static float f(int i)
{
	int a = i % 40;
	int b = (i / 40) % 6;
	switch (b) {
		case 0:
		case 1:
			return 0.0f;
		case 3:
		case 4:
			return 1.0f;
		case 2:
			return (a / 40.0f);
		case 5:
			return 1.0f - (a / 40.0f);
		default:
			return 0.0f;
	}
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
			      void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

int main(int argc, char **argv)
{
	int fd = -1;
	if (argc >= 2) {
		fd = open(argv[1], O_RDWR);
		if (fd < 0) {
			bs_debug_error("failed to open card %s", argv[1]);
			return 1;
		}
	}
	else {
		fd = bs_drm_open_main_display();
		if (fd < 0) {
			bs_debug_error("failed to open card for display");
			return 1;
		}
	}

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm");
		return 1;
	}

	struct bs_drm_pipe pipe = {0};
	if (!bs_drm_pipe_make(fd, &pipe)) {
		bs_debug_error("failed to make pipe");
		return 1;
	}

	drmModeConnector *connector = drmModeGetConnector(fd, pipe.connector_id);
	assert(connector);
	drmModeModeInfo *mode = &connector->modes[0];

	struct bs_egl *egl = bs_egl_new();
	if (!bs_egl_setup(egl)) {
		bs_debug_error("failed to setup egl context");
		return 1;
	}

	struct gbm_bo *bos[2];
	uint32_t ids[2];
	struct bs_egl_fb *egl_fbs[2];
	for (size_t fb_index = 0; fb_index < 2; fb_index++) {
		bos[fb_index] =
		    gbm_bo_create(gbm, mode->hdisplay, mode->vdisplay, GBM_FORMAT_XRGB8888,
				  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (bos[fb_index] == NULL) {
			bs_debug_error("failed to allocate framebuffer");
			return 1;
		}

		ids[fb_index] = bs_drm_fb_create_gbm(bos[fb_index]);
		if (ids[fb_index] == 0) {
			bs_debug_error("failed to create framebuffer id");
			return 1;
		}

		EGLImageKHR egl_image = bs_egl_image_create_gbm(egl, bos[fb_index]);
		if (egl_image == EGL_NO_IMAGE_KHR) {
			bs_debug_error("failed to create EGLImageKHR from framebuffer");
			return 1;
		}

		egl_fbs[fb_index] = bs_egl_fb_new(egl, egl_image);
		if (!egl_fbs[fb_index]) {
			bs_debug_error("failed to create framebuffer from EGLImageKHR");
			return 1;
		}
	}

	int ret = drmModeSetCrtc(fd, pipe.crtc_id, ids[0], 0 /* x */, 0 /* y */, &pipe.connector_id,
				 1 /* connector count */, mode);
	if (ret) {
		bs_debug_error("failed to set CRTC");
		return 1;
	}

	GLuint program = solid_shader_create();
	if (!program) {
		bs_debug_error("failed to create solid shader");
		return 1;
	}

	int fb_idx = 1;
	for (int i = 0; i <= 500; i++) {
		int waiting_for_flip = 1;
		// clang-format off
		GLfloat verts[] = {
			0.0f, -0.5f, 0.0f,
			-0.5f, 0.5f, 0.0f,
			0.5f, 0.5f, 0.0f
		};
		GLfloat colors[] = {
			1.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 1.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f, 1.0f
		};
		// clang-format on

		glBindFramebuffer(GL_FRAMEBUFFER, bs_egl_fb_name(egl_fbs[fb_idx]));
		glViewport(0, 0, (GLint)mode->hdisplay, (GLint)mode->vdisplay);

		glClearColor(f(i), f(i + 80), f(i + 160), 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(program);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, verts);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		usleep(1e6 / 120); /* 120 Hz */
		glFinish();
		ret = drmModePageFlip(fd, pipe.crtc_id, ids[fb_idx], DRM_MODE_PAGE_FLIP_EVENT,
				      &waiting_for_flip);
		if (ret) {
			bs_debug_error("failed page flip: %d", ret);
			return 1;
		}

		while (waiting_for_flip) {
			drmEventContext evctx = {
			    .version = DRM_EVENT_CONTEXT_VERSION,
			    .page_flip_handler = page_flip_handler,
			};

			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(0, &fds);
			FD_SET(fd, &fds);

			ret = select(fd + 1, &fds, NULL, NULL, NULL);
			if (ret < 0) {
				bs_debug_error("select err: %s", strerror(errno));
				return 1;
			}
			else if (ret == 0) {
				bs_debug_error("select timeout");
				return 1;
			}
			else if (FD_ISSET(0, &fds)) {
				bs_debug_error("user interrupted");
				return 1;
			}
			ret = drmHandleEvent(fd, &evctx);
			if (ret) {
				bs_debug_error("failed to wait for page flip: %d", ret);
				return 1;
			}
		}
		fb_idx = fb_idx ^ 1;
	}

	return 0;
}
