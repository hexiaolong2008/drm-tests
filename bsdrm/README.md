BS DRM
===

BS DRM is a library to factor out most of the boilerplate found in programs
written against DRM.

Dependencies
---
- Clang or GCC with `-std=gnu99`
- `libgbm`
- `libdrm`

Features
---
- Opens DRM devices
- Creates display pipelines
- Allocates buffer objects with framebuffers
- Dumb maps buffer objects

Principles
---
(in order of priority)

- Useful
- Consistent and guessable API
- Easy to read
- Code is documentation
- 0 warnings from Clang or GCC
- Make errors easy to debug
- Fun

Build Example
---
Define `CC` `CFLAGS` and `LDFLAGS` for your device. Then run the following from the top level directory:
```
${CC} ${CFLAGS} -c src/drm_fb.c -o drm_fb.o || exit 1
${CC} ${CFLAGS} -c src/drm_open.c -o drm_open.o || exit 1
${CC} ${CFLAGS} -c src/drm_pipe.c -o drm_pipe.o || exit 1
${CC} ${CFLAGS} -c src/dumb_mmap.c -o dumb_mmap.o || exit 1
${CC} ${CFLAGS} -c src/pipe.c -o pipe.o || exit 1
${CC} ${CFLAGS} -c example/stripe.c -o stripe.o || exit 1
${CC} ${LDFLAGS} \
    pipe.o \
    drm_pipe.o \
    drm_fb.o \
    drm_open.o \
    dumb_mmap.o \
    stripe.o -o bstest || exit 1
```

Upcoming Features
---
- Vsync
- Atomic Commit
- Multi-card and multi-monitor
- Keyboard/Mouse input for debugging
- Dump buffer output for debugging

What does BS stand for?
---
BS does't stand for anything.
