/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bs_drm.h"

static bool has_extension(const char *extension, const char *extensions);
static const char *get_egl_error();
static const char *get_gl_framebuffer_error();

struct bs_egl {
	bool setup;
	EGLDisplay display;
	EGLContext ctx;

	// Names are the original gl/egl function names with the prefix chopped off.
	PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC DestroyImageKHR;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC EGLImageTargetTexture2DOES;
};

struct bs_egl_fb {
	GLuint tex;
	GLuint fb;
};

struct bs_egl *bs_egl_new()
{
	struct bs_egl *self = calloc(1, sizeof(struct bs_egl));
	assert(self);
	self->display = EGL_NO_DISPLAY;
	self->ctx = EGL_NO_CONTEXT;
	return self;
}

void bs_egl_destroy(struct bs_egl **egl)
{
	assert(egl);
	struct bs_egl *self = *egl;
	assert(self);

	if (self->ctx != EGL_NO_CONTEXT) {
		assert(self->display != EGL_NO_DISPLAY);
		eglDestroyContext(self->display, self->ctx);
	}

	if (self->display != EGL_NO_DISPLAY)
		eglTerminate(self->display);

	free(self);
	*egl = NULL;
}

bool bs_egl_setup(struct bs_egl *self)
{
	assert(self);
	assert(!self->setup);

	self->CreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	self->DestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	self->EGLImageTargetTexture2DOES =
	    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!self->CreateImageKHR || !self->DestroyImageKHR || !self->EGLImageTargetTexture2DOES) {
		bs_debug_error(
		    "eglGetProcAddress returned NULL for a required extension entry point.");
		return false;
	}

	self->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (self->display == EGL_NO_DISPLAY) {
		bs_debug_error("failed to get egl display");
		return false;
	}

	if (!eglInitialize(self->display, NULL /* ignore version */, NULL /* ignore version */)) {
		bs_debug_error("failed to initialize egl: %s\n", get_egl_error());
		return false;
	}

	// Get any EGLConfig. We need one to create a context, but it isn't used to create any
	// surfaces.
	const EGLint config_attrib = EGL_NONE;
	EGLConfig egl_config;
	EGLint num_configs;
	if (!eglChooseConfig(self->display, &config_attrib, &egl_config, 1,
			     &num_configs /* unused but can't be null */)) {
		bs_debug_error("eglChooseConfig() failed with error: %s", get_egl_error());
		goto terminate_display;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		bs_debug_error("failed to bind OpenGL ES: %s", get_egl_error());
		goto terminate_display;
	}

	const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

	self->ctx = eglCreateContext(self->display, egl_config,
				     EGL_NO_CONTEXT /* no shared context */, context_attribs);
	if (self->ctx == EGL_NO_CONTEXT) {
		bs_debug_error("failed to create OpenGL ES Context: %s", get_egl_error());
		goto terminate_display;
	}

	if (!eglMakeCurrent(self->display, EGL_NO_SURFACE /* no default draw surface */,
			    EGL_NO_SURFACE /* no default draw read */, self->ctx)) {
		bs_debug_error("failed to make the OpenGL ES Context current: %s", get_egl_error());
		goto destroy_context;
	}

	const char *egl_extensions = eglQueryString(self->display, EGL_EXTENSIONS);
	if (!has_extension("EGL_KHR_image_base", egl_extensions)) {
		bs_debug_error("EGL_KHR_image_base extension not supported");
		goto destroy_context;
	}
	if (!has_extension("EGL_EXT_image_dma_buf_import", egl_extensions)) {
		bs_debug_error("EGL_EXT_image_dma_buf_import extension not supported");
		goto destroy_context;
	}

	const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (!has_extension("GL_OES_EGL_image", gl_extensions)) {
		bs_debug_error("GL_OES_EGL_image extension not supported");
		goto destroy_context;
	}

	self->setup = true;

	return true;

destroy_context:
	eglDestroyContext(self->display, self->ctx);
terminate_display:
	eglTerminate(self->display);
	self->display = EGL_NO_DISPLAY;
	return false;
}

bool bs_egl_make_current(struct bs_egl *self)
{
	assert(self);
	assert(self->display != EGL_NO_DISPLAY);
	assert(self->ctx != EGL_NO_CONTEXT);
	return eglMakeCurrent(self->display, EGL_NO_SURFACE /* No default draw surface */,
			      EGL_NO_SURFACE /* No default draw read */, self->ctx);
}

EGLImageKHR bs_egl_image_create(struct bs_egl *self, int prime_fd, int width, int height,
				uint32_t format, int pitch, int offset)
{
	assert(self);
	assert(self->CreateImageKHR);
	assert(self->display != EGL_NO_DISPLAY);
	const EGLint khr_image_attrs[] = {EGL_DMA_BUF_PLANE0_FD_EXT,
					  prime_fd,
					  EGL_WIDTH,
					  width,
					  EGL_HEIGHT,
					  height,
					  EGL_LINUX_DRM_FOURCC_EXT,
					  (int)format,
					  EGL_DMA_BUF_PLANE0_PITCH_EXT,
					  pitch,
					  EGL_DMA_BUF_PLANE0_OFFSET_EXT,
					  offset,
					  EGL_NONE};

	EGLImageKHR image =
	    self->CreateImageKHR(self->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
				 NULL /* no client buffer */, khr_image_attrs);

	if (image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to make image from target buffer: %s", get_egl_error());
		return EGL_NO_IMAGE_KHR;
	}

	return image;
}

EGLImageKHR bs_egl_image_create_gbm(struct bs_egl *self, struct gbm_bo *bo)
{
	assert(bo);
	int fd = gbm_bo_get_fd(bo);
	if (fd < 0) {
		bs_debug_error("failed to get fb for bo: %d", fd);
		return EGL_NO_IMAGE_KHR;
	}
	return bs_egl_image_create(self, fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
				   gbm_bo_get_format(bo), gbm_bo_get_stride(bo), 0 /* no offset */);
}

void bs_egl_image_destroy(struct bs_egl *self, EGLImageKHR *image)
{
	assert(self);
	assert(image);
	assert(*image != EGL_NO_IMAGE_KHR);
	assert(self->DestroyImageKHR);
	self->DestroyImageKHR(self->display, *image);
	*image = EGL_NO_IMAGE_KHR;
}

struct bs_egl_fb *bs_egl_fb_new(struct bs_egl *self, EGLImageKHR image)
{
	assert(self);
	assert(self->EGLImageTargetTexture2DOES);

	struct bs_egl_fb *fb = calloc(1, sizeof(struct bs_egl_fb));
	assert(fb);

	glGenTextures(1, &fb->tex);
	glBindTexture(GL_TEXTURE_2D, fb->tex);
	self->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &fb->fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb->fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->tex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		bs_debug_error("failed framebuffer check for created target buffer: %s",
			       get_gl_framebuffer_error());
		glDeleteFramebuffers(1, &fb->fb);
		glDeleteTextures(1, &fb->tex);
		free(fb);
		return NULL;
	}

	return fb;
}

void bs_egl_fb_destroy(struct bs_egl_fb **fb)
{
	assert(fb);
	struct bs_egl_fb *self = *fb;
	assert(self);

	glDeleteFramebuffers(1, &self->fb);
	glDeleteTextures(1, &self->tex);

	free(self);
	*fb = NULL;
}

GLuint bs_egl_fb_name(struct bs_egl_fb *self)
{
	assert(self);
	return self->fb;
}

static bool has_extension(const char *extension, const char *extensions)
{
	const char *start, *where, *terminator;
	start = extensions;
	for (;;) {
		where = (char *)strstr((const char *)start, extension);
		if (!where)
			break;
		terminator = where + strlen(extension);
		if (where == start || *(where - 1) == ' ')
			if (*terminator == ' ' || *terminator == '\0')
				return true;
		start = terminator;
	}
	return false;
}

static const char *get_egl_error()
{
	switch (eglGetError()) {
		case EGL_SUCCESS:
			return "EGL_SUCCESS";
		case EGL_NOT_INITIALIZED:
			return "EGL_NOT_INITIALIZED";
		case EGL_BAD_ACCESS:
			return "EGL_BAD_ACCESS";
		case EGL_BAD_ALLOC:
			return "EGL_BAD_ALLOC";
		case EGL_BAD_ATTRIBUTE:
			return "EGL_BAD_ATTRIBUTE";
		case EGL_BAD_CONTEXT:
			return "EGL_BAD_CONTEXT";
		case EGL_BAD_CONFIG:
			return "EGL_BAD_CONFIG";
		case EGL_BAD_CURRENT_SURFACE:
			return "EGL_BAD_CURRENT_SURFACE";
		case EGL_BAD_DISPLAY:
			return "EGL_BAD_DISPLAY";
		case EGL_BAD_SURFACE:
			return "EGL_BAD_SURFACE";
		case EGL_BAD_MATCH:
			return "EGL_BAD_MATCH";
		case EGL_BAD_PARAMETER:
			return "EGL_BAD_PARAMETER";
		case EGL_BAD_NATIVE_PIXMAP:
			return "EGL_BAD_NATIVE_PIXMAP";
		case EGL_BAD_NATIVE_WINDOW:
			return "EGL_BAD_NATIVE_WINDOW";
		case EGL_CONTEXT_LOST:
			return "EGL_CONTEXT_LOST";
		default:
			return "EGL_???";
	}
}

static const char *get_gl_framebuffer_error()
{
	switch (glCheckFramebufferStatus(GL_FRAMEBUFFER)) {
		case GL_FRAMEBUFFER_COMPLETE:
			return "GL_FRAMEBUFFER_COMPLETE";
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
			return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
			return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
		case GL_FRAMEBUFFER_UNSUPPORTED:
			return "GL_FRAMEBUFFER_UNSUPPORTED";
		case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
			return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
		default:
			return "GL_FRAMEBUFFER_???";
	}
}
