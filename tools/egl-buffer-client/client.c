// D1 diagnostic (Phase 3C): a raw Wayland client with N independent linear scanout buffers, so the source buffer
// cycle is longer than the compositor's three-slot cycle. It binds wl_compositor, xdg_wm_base and
// zwp_linux_dmabuf_v1, renders an asymmetric pattern per frame, and only reuses a buffer after wl_buffer.release.
// Test-only; not part of the image.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

// buffer size: 1240x1080 (the logical output) unless CLIENT_W/CLIENT_H say otherwise (the letterbox test)
static int W = 1240, H = 1080;
#define N 6

struct Buf { struct gbm_bo* bo; struct wl_buffer* wl; EGLImage img; GLuint tex, fbo; int free; };

struct State
{
	struct wl_display* dpy;
	struct wl_registry* reg;
	struct wl_compositor* comp;
	struct xdg_wm_base* xdg;
	struct zwp_linux_dmabuf_v1* dmabuf;
	struct wl_seat* seat;
	struct wl_keyboard* kb;
	struct wl_pointer* ptr;
	struct wl_touch* touch;
	struct wl_surface* surf;
	struct xdg_surface* xs;
	struct xdg_toplevel* top;
	int configured, quit;
	struct gbm_device* gbm;
	EGLDisplay edpy;
	EGLContext ctx;
	PFNEGLCREATEIMAGEKHRPROC createImage;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetTexture;
	struct Buf buf[N];
};

static double nowMs(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0; }

static void bufRelease(void* data, struct wl_buffer* b) { (void)b; ((struct Buf*)data)->free = 1; }
static const struct wl_buffer_listener bufListener = { .release = bufRelease };

static void xsConfigure(void* data, struct xdg_surface* xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	((struct State*)data)->configured = 1;
}
static const struct xdg_surface_listener xsListener = { .configure = xsConfigure };

static void topConfigure(void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* st) { (void)data; (void)t; (void)w; (void)h; (void)st; }
static void topClose(void* data, struct xdg_toplevel* t) { (void)t; ((struct State*)data)->quit = 1; }
static void topBounds(void* data, struct xdg_toplevel* t, int32_t w, int32_t h) { (void)data; (void)t; (void)w; (void)h; }
static const struct xdg_toplevel_listener topListener = { .configure = topConfigure, .close = topClose, .configure_bounds = topBounds, .wm_capabilities = NULL };

static void wmPing(void* data, struct xdg_wm_base* wm, uint32_t serial) { (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wmListener = { .ping = wmPing };

// 3E.2: print what the compositor's seat sends (keyboard focus, keys and modifiers; pointer/touch enter/leave).
static void kbKeymap(void* data, struct wl_keyboard* kb, uint32_t format, int32_t fd, uint32_t size)
{
	(void)data; (void)kb;
	printf("keyboard keymap format=%u size=%u\n", format, size);
	fflush(stdout);
	close(fd);
}
static void kbEnter(void* data, struct wl_keyboard* kb, uint32_t serial, struct wl_surface* surf, struct wl_array* keys)
{
	(void)data; (void)kb; (void)surf;
	printf("keyboard enter serial=%u keys=%u\n", serial, (unsigned)(keys->size / sizeof(uint32_t)));
	fflush(stdout);
}
static void kbLeave(void* data, struct wl_keyboard* kb, uint32_t serial, struct wl_surface* surf)
{
	(void)data; (void)kb; (void)surf;
	printf("keyboard leave serial=%u\n", serial);
	fflush(stdout);
}
static void kbKey(void* data, struct wl_keyboard* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	(void)data; (void)kb; (void)serial; (void)time;
	printf("key event key=%u state=%u\n", key, state);
	fflush(stdout);
}
static void kbMods(void* data, struct wl_keyboard* kb, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)data; (void)kb; (void)serial;
	printf("keyboard modifiers dep=0x%x lat=0x%x lock=0x%x group=%u\n", dep, lat, lock, group);
	fflush(stdout);
}
static void kbRepeat(void* data, struct wl_keyboard* kb, int32_t rate, int32_t delay) { (void)data; (void)kb; (void)rate; (void)delay; }
static const struct wl_keyboard_listener kbListener = {
	.keymap = kbKeymap, .enter = kbEnter, .leave = kbLeave, .key = kbKey, .modifiers = kbMods, .repeat_info = kbRepeat,
};

static void ptrEnter(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* surf, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)surf;
	printf("pointer enter serial=%u %d,%d\n", serial, (int)wl_fixed_to_double(x), (int)wl_fixed_to_double(y));
	fflush(stdout);
}
static void ptrLeave(void* data, struct wl_pointer* p, uint32_t serial, struct wl_surface* surf)
{
	(void)data; (void)p; (void)surf;
	printf("pointer leave serial=%u\n", serial);
	fflush(stdout);
}
static void ptrMotion(void* data, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)time;
	printf("pointer motion %d,%d\n", (int)wl_fixed_to_double(x), (int)wl_fixed_to_double(y));
	fflush(stdout);
}
static void ptrButton(void* data, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)data; (void)p; (void)time;
	printf("pointer button %u state=%u serial=%u\n", button, state, serial);
	fflush(stdout);
}
static void ptrAxis(void* data, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)p; (void)time;
	printf("pointer axis %u %d\n", axis, (int)wl_fixed_to_double(value));
	fflush(stdout);
}
static void ptrFrame(void* data, struct wl_pointer* p) { (void)data; (void)p; }
static const struct wl_pointer_listener ptrListener = {
	.enter = ptrEnter, .leave = ptrLeave, .motion = ptrMotion, .button = ptrButton, .axis = ptrAxis, .frame = ptrFrame,
};

static void touchDown(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, struct wl_surface* surf, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)t; (void)surf; (void)time;
	printf("touch down id=%d serial=%u %d,%d\n", id, serial, (int)wl_fixed_to_double(x), (int)wl_fixed_to_double(y));
	fflush(stdout);
}
static void touchUp(void* data, struct wl_touch* t, uint32_t serial, uint32_t time, int32_t id)
{
	(void)data; (void)t; (void)time;
	printf("touch up id=%d serial=%u\n", id, serial);
	fflush(stdout);
}
static void touchMotion(void* data, struct wl_touch* t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)t; (void)time;
	printf("touch motion id=%d %d,%d\n", id, (int)wl_fixed_to_double(x), (int)wl_fixed_to_double(y));
	fflush(stdout);
}
static void touchFrame(void* data, struct wl_touch* t) { (void)data; (void)t; }
static void touchCancel(void* data, struct wl_touch* t)
{
	(void)data; (void)t;
	printf("touch cancel\n");
	fflush(stdout);
}
static const struct wl_touch_listener touchListener = {
	.down = touchDown, .up = touchUp, .motion = touchMotion, .frame = touchFrame, .cancel = touchCancel,
};

static void seatCaps(void* data, struct wl_seat* seat, uint32_t caps)
{
	struct State* s = (struct State*)data;
	printf("seat caps=0x%x\n", caps);
	fflush(stdout);
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s->kb)
	{
		s->kb = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(s->kb, &kbListener, s);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s->ptr)
	{
		s->ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(s->ptr, &ptrListener, s);
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !s->touch)
	{
		s->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(s->touch, &touchListener, s);
	}
}
static void seatName(void* data, struct wl_seat* seat, const char* name) { (void)data; (void)seat; (void)name; }
static const struct wl_seat_listener seatListener = { .capabilities = seatCaps, .name = seatName };

static void regGlobal(void* data, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver)
{
	struct State* s = (struct State*)data;
	if (!strcmp(iface, wl_compositor_interface.name)) s->comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) s->xdg = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) s->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, wl_seat_interface.name))
	{
		s->seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
		wl_seat_add_listener(s->seat, &seatListener, s);
	}
}
static void regRemove(void* data, struct wl_registry* reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener regListener = { .global = regGlobal, .global_remove = regRemove };

static int openDrm(void)
{
	const char* paths[] = { "/dev/dri/renderD128", "/dev/dri/card1", "/dev/dri/card0" };
	for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++)
	{
		int fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) { fprintf(stderr, "drm: %s\n", paths[i]); return fd; }
	}
	return -1;
}

static void renderFrame(struct State* s, struct Buf* b, int frame)
{
	glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
	glViewport(0, 0, W, H);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glClearColor((frame % 3) * 0.25f, ((frame / 3) % 3) * 0.25f, 0.1f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < 10; i++)
		if (frame & (1 << i))
		{
			glScissor(80 + i * 100, 80, 60, 240);
			glClearColor(1, 1, 1, 1);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	// four distinct corners (GL's origin is bottom-left)
	glScissor(0, 0, 80, 80); glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
	glScissor(W - 80, 0, 80, 80); glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
	glScissor(0, H - 80, 80, 80); glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
	glScissor(W - 80, H - 80, 80, 80); glClearColor(1, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glFinish();
}

int main(int argc, char** argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	if (getenv("CLIENT_W")) W = atoi(getenv("CLIENT_W"));
	if (getenv("CLIENT_H")) H = atoi(getenv("CLIENT_H"));
	if (W < 80 || H < 80) { fprintf(stderr, "CLIENT_W/CLIENT_H must be >= 80\n"); return 1; }
	struct State s;
	memset(&s, 0, sizeof s);
	s.dpy = wl_display_connect(NULL);
	if (!s.dpy) { fprintf(stderr, "wayland connect failed\n"); return 1; }
	s.reg = wl_display_get_registry(s.dpy);
	wl_registry_add_listener(s.reg, &regListener, &s);
	wl_display_roundtrip(s.dpy);
	if (!s.comp || !s.xdg || !s.dmabuf) { fprintf(stderr, "missing globals\n"); return 1; }
	xdg_wm_base_add_listener(s.xdg, &wmListener, &s);

	int drmFd = openDrm();
	if (drmFd < 0) { fprintf(stderr, "no drm node\n"); return 1; }
	s.gbm = gbm_create_device(drmFd);
	if (!s.gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	s.edpy = getPlatform ? getPlatform(EGL_PLATFORM_GBM_KHR, s.gbm, NULL) : eglGetDisplay((EGLNativeDisplayType)s.gbm);
	if (!eglInitialize(s.edpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	if (!eglChooseConfig(s.edpy, attrs, &cfg, 1, &n) || n < 1) { fprintf(stderr, "eglChooseConfig failed\n"); return 1; }
	EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	s.ctx = eglCreateContext(s.edpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
	if (s.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(s.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.ctx)) { fprintf(stderr, "EGL context failed\n"); return 1; }
	s.createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	s.targetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!s.createImage || !s.targetTexture) { fprintf(stderr, "EGLImage entry points missing\n"); return 1; }

	for (int i = 0; i < N; i++)
	{
		uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
		struct gbm_bo* bo = gbm_bo_create_with_modifiers2(s.gbm, W, H, GBM_FORMAT_XRGB8888, mods, 1, GBM_BO_USE_RENDERING);
		if (!bo) { fprintf(stderr, "gbm bo %d failed: %s\n", i, strerror(errno)); return 1; }
		int fd = gbm_bo_get_fd_for_plane(bo, 0);
		uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0), off = gbm_bo_get_offset(bo, 0);
		EGLint ia[] = {
			EGL_WIDTH, W, EGL_HEIGHT, H,
			EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
			EGL_DMA_BUF_PLANE0_FD_EXT, fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)off,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
			EGL_NONE
		};
		s.buf[i].bo = bo;
		s.buf[i].img = s.createImage(s.edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
		if (s.buf[i].img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "EGLImage %d failed 0x%x\n", i, eglGetError()); return 1; }
		glGenTextures(1, &s.buf[i].tex);
		glBindTexture(GL_TEXTURE_2D, s.buf[i].tex);
		s.targetTexture(GL_TEXTURE_2D, s.buf[i].img);
		glGenFramebuffers(1, &s.buf[i].fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, s.buf[i].fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.buf[i].tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO %d incomplete\n", i); return 1; }
		struct zwp_linux_buffer_params_v1* p = zwp_linux_dmabuf_v1_create_params(s.dmabuf);
		zwp_linux_buffer_params_v1_add(p, fd, 0, off, stride, 0, 0);
		s.buf[i].wl = zwp_linux_buffer_params_v1_create_immed(p, W, H, DRM_FORMAT_XRGB8888, 0);
		zwp_linux_buffer_params_v1_destroy(p);
		wl_buffer_add_listener(s.buf[i].wl, &bufListener, &s.buf[i]);
		close(fd);
		s.buf[i].free = 1;
		fprintf(stderr, "buffer %d: stride=%u off=%u\n", i, stride, off);
	}

	s.surf = wl_compositor_create_surface(s.comp);
	s.xs = xdg_wm_base_get_xdg_surface(s.xdg, s.surf);
	xdg_surface_add_listener(s.xs, &xsListener, &s);
	s.top = xdg_surface_get_toplevel(s.xs);
	xdg_toplevel_add_listener(s.top, &topListener, &s);
	xdg_toplevel_set_title(s.top, "egl-buffer-client");
	wl_surface_commit(s.surf);
	while (!s.configured && wl_display_dispatch(s.dpy) >= 0 && !s.quit) {}

	int frame = 0;
	double end = nowMs() + secs * 1000.0;
	while (!s.quit && nowMs() < end)
	{
		int idx = -1;
		for (int i = 0; i < N; i++) if (s.buf[i].free) { idx = i; break; }
		if (idx < 0) { wl_display_dispatch(s.dpy); continue; }
		s.buf[idx].free = 0;
		renderFrame(&s, &s.buf[idx], frame);
		wl_surface_attach(s.surf, s.buf[idx].wl, 0, 0);
		wl_surface_damage_buffer(s.surf, 0, 0, W, H);
		wl_surface_commit(s.surf);
		frame++;
		printf("commit frame %d buffer %d\n", frame, idx);
		fflush(stdout);
		wl_display_dispatch_pending(s.dpy);
		wl_display_flush(s.dpy);
		usleep(16000);
	}
	printf("done %d frames\n", frame);
	return 0;
}
