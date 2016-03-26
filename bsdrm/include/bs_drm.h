/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BS_DRM_H__
#define __BS_DRM_H__

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// debug.c
__attribute__((format(printf, 5, 6))) void bs_debug_print(const char *prefix, const char *func,
							  const char *file, int line,
							  const char *format, ...);
#define bs_debug_error(...)                                                         \
	do {                                                                        \
		bs_debug_print("ERROR", __func__, __FILE__, __LINE__, __VA_ARGS__); \
	} while (0)

int64_t bs_debug_gettime_ns();

// pipe.c
typedef bool (*bs_make_pipe_piece)(void *context, void *out);

bool bs_pipe_make(void *context, bs_make_pipe_piece *pieces, size_t piece_count, void *out_pipe,
		  size_t pipe_size);

// open.c
extern const uint32_t bs_open_rank_skip;

// A return value of true causes enumeration to end immediately. fd is always
// closed after the callback.
typedef bool (*bs_open_enumerate_func)(void *user, int fd);

// A return value of true causes the filter to return the given fd.
typedef bool (*bs_open_filter_func)(int fd);

// The fd with the lowest (magnitude) rank is returned. A fd with rank UINT32_MAX is skipped. A fd
// with rank 0 ends the enumeration early and is returned. On a tie, the fd returned will be
// arbitrarily chosen from the set of lowest rank fds.
typedef uint32_t (*bs_open_rank_func)(int fd);

void bs_open_enumerate(const char *format, unsigned start, unsigned end,
		       bs_open_enumerate_func body, void *user);
int bs_open_filtered(const char *format, unsigned start, unsigned end, bs_open_filter_func filter);
int bs_open_ranked(const char *format, unsigned start, unsigned end, bs_open_rank_func rank);

// drm_pipe.c
struct bs_drm_pipe {
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
};

bool bs_drm_pipe_make(int fd, struct bs_drm_pipe *pipe);

// drm_fb.c
uint32_t bs_drm_fb_create_gbm(struct gbm_bo *bo);

// drm_open.c
// Opens an arbitrary display's card.
int bs_drm_open_for_display();
// Opens the main display's card. This falls back to bs_drm_open_for_display().
int bs_drm_open_main_display();
int bs_drm_open_vgem();

// dumb_mmap.c
void *bs_dumb_mmap(int fd, uint32_t handle, size_t size);
void *bs_dumb_mmap_gbm(struct gbm_bo *bo);
int bs_dumb_unmmap_gbm(struct gbm_bo *bo, void *addr);

// egl.c
struct bs_egl;
struct bs_egl_fb;

struct bs_egl *bs_egl_new();
void bs_egl_destroy(struct bs_egl **egl);
bool bs_egl_setup(struct bs_egl *self);
bool bs_egl_make_current(struct bs_egl *self);

EGLImageKHR bs_egl_image_create(struct bs_egl *self, int prime_fd, int width, int height,
				uint32_t format, int pitch, int offset);
EGLImageKHR bs_egl_image_create_gbm(struct bs_egl *self, struct gbm_bo *bo);
void bs_egl_image_destroy(struct bs_egl *self, EGLImageKHR *image);

struct bs_egl_fb *bs_egl_fb_new(struct bs_egl *self, EGLImageKHR image);
void bs_egl_fb_destroy(struct bs_egl_fb **fb);
GLuint bs_egl_fb_name(struct bs_egl_fb *self);

// app.c
struct bs_app;

struct bs_app *bs_app_new();
void bs_app_destroy(struct bs_app **app);
int bs_app_fd(struct bs_app *self);
size_t bs_app_fb_count(struct bs_app *self);
void bs_app_set_fb_count(struct bs_app *self, size_t fb_count);
struct gbm_bo *bs_app_fb_bo(struct bs_app *self, size_t index);
uint32_t bs_app_fb_id(struct bs_app *self, size_t index);
bool bs_app_setup(struct bs_app *self);
int bs_app_display_fb(struct bs_app *self, size_t index);

#endif
