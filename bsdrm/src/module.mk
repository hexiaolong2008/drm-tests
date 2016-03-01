# Copyright 2016 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS += -std=gnu99 -I$(SRC)/bsdrm/include

CC_STATIC_LIBRARY(libbsdrm.pic.a): \
  bsdrm/src/app.o \
  bsdrm/src/debug.o \
  bsdrm/src/drm_fb.o \
  bsdrm/src/drm_open.o \
  bsdrm/src/drm_pipe.o \
  bsdrm/src/dumb_mmap.o \
  bsdrm/src/egl.o \
  bsdrm/src/gl.o \
  bsdrm/src/open.o \
  bsdrm/src/pipe.o
