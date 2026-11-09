// dmabuf-capture: a test-only Wayland server that receives a client's real DMA-BUF and tries to import and
// scan it out on the DRM planes. It is phase 2 of docs/PLAN_DRM_COMPOSITOR.md and is NOT the compositor: no menu,
// no overlay UI, no integration; it exists to answer "which buffer does a real client hand over, and does the DPU
// accept/rotate/scan it".
//
//   dmabuf-capture --socket wayland-dmabuf --seconds 20 [--drm] [--test] [--live]
//                  [--card /dev/dri/cardN] [--client <cmd> [args...]]
//
//   (no --drm)      capture metadata only (safe with the menu running; the client renders offscreen)
//   --drm --test    take DRM master (menu stopped) and run TEST_ONLY import/primary/overlay/rotation
//   --drm --live    also scan the accepted buffer out for a few seconds (bounded; the wrapper restores the menu)
//
// The DRM work is deliberately gated: a live commit is only attempted after the matching TEST_ONLY row passed.
// Output is line-oriented for a saved hardware report.

#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "wayland-drm-server-protocol.h"

#define MAX_PLANES 4
#define MAX_BUFFERS 32
#define MAX_ADV 64

struct buffer
{
	struct wl_resource *res;
	bool shm;
	int fd[MAX_PLANES];
	uint32_t offset[MAX_PLANES], stride[MAX_PLANES];
	uint64_t modifier;
	uint32_t format, width, height, flags;
	int nplanes;
	uint32_t fb;
	bool imported, import_failed;
	struct wl_list link;
};

struct surface
{
	struct wl_resource *res, *xdg, *toplevel;
	struct buffer *pending, *current;
	struct wl_resource *frame_cb;
	bool configured;
};

static struct wl_display *display;
static struct wl_event_loop *loop;
static struct wl_event_source *timer, *drm_src;
static struct wl_list buffers;
static struct surface *the_surface;
static const char *socket_name = "wayland-dmabuf";
static int run_seconds = 20;
static int drm_fd = -1;
static bool use_drm, do_test, do_live, opt_linear;
static uint32_t target_conn, target_crtc, target_crtc_index;
static drmModeModeInfo target_mode;
static bool have_mode;
static uint32_t mode_blob;
struct plane
{
	uint32_t id, type, possible_crtcs;
	uint32_t crtc_id, fb_id, src_x, src_y, src_w, src_h, crt_x, crt_y, crt_w, crt_h;
	uint32_t zpos_id, alpha_id, blend_id, rot_id;
	uint64_t zpos_max;
	bool zpos_mutable;
};
static struct plane primary, overlay;
static bool have_overlay;
static uint32_t overlay_fb, overlay_w = 600, overlay_h = 200;
static bool rotation_ok, rotation_tested;
static bool overlay_on;
static struct buffer *pending_buf, *current_buf;
static bool flip_pending;
static int frames_live;
static pid_t client_pid;
static bool live_started, tested;
static int buf_count, buf_unique;
static uint32_t last_fmt, last_w, last_h;
static uint64_t last_mod;
static int last_planes;
static const char *report_client = "unknown";
static char drm_name[64] = "";
static dev_t target_dev;
struct adv { uint32_t format; uint64_t modifier; };
static struct adv adv[MAX_ADV];
static int nadv;


static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static const char *fcc(uint32_t f, char out[16])
{
	out[0] = f & 0xff; out[1] = (f >> 8) & 0xff; out[2] = (f >> 16) & 0xff; out[3] = (f >> 24) & 0xff; out[4] = 0;
	for (int i = 0; i < 4; i++)
		if (out[i] < 0x20 || out[i] > 0x7e) { snprintf(out, 16, "0x%08" PRIx32, f); break; }
	return out;
}

static const char *mod_name(uint64_t m)
{
	static char buf[64];
	const char *n = drmGetFormatModifierName(m);
	snprintf(buf, sizeof buf, "%s", n ? n : "?");
	free((void *)n);
	return buf;
}

// ---- DRM -----------------------------------------------------------------------------------------------------------

static const char *find_card(void)
{
	static char path[64];
	for (int dsi = 1; dsi >= 0; dsi--)
	{
		DIR *d = opendir("/sys/class/drm");
		if (!d) break;
		struct dirent *e;
		while ((e = readdir(d)))
		{
			if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
			if (dsi && !strstr(e->d_name, "-DSI")) continue;
			char f[320], st[32] = "";
			snprintf(f, sizeof f, "/sys/class/drm/%s/status", e->d_name);
			FILE *sf = fopen(f, "r");
			if (sf) { if (fgets(st, sizeof st, sf)) {} fclose(sf); }
			if (strncmp(st, "connected", 9)) continue;
			int n = (int)(strchr(e->d_name, '-') - e->d_name);
			snprintf(path, sizeof path, "/dev/dri/%.*s", n, e->d_name);
			closedir(d);
			return path;
		}
		closedir(d);
	}
	return "/dev/dri/card0";
}

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectProperties *p = drmModeObjectGetProperties(drm_fd, obj, type);
	if (!p) return 0;
	uint32_t id = 0;
	for (uint32_t i = 0; i < p->count_props && !id; i++)
	{
		drmModePropertyPtr pr = drmModeGetProperty(drm_fd, p->props[i]);
		if (pr && !strcmp(pr->name, name)) id = pr->prop_id;
		if (pr) drmModeFreeProperty(pr);
	}
	drmModeFreeObjectProperties(p);
	return id;
}

static void drm_target(void)
{
	drmModeRes *res = drmModeGetResources(drm_fd);
	if (!res) return;
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors && !conn; i++)
	{
		drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) conn = c;
		else if (c) drmModeFreeConnector(c);
	}
	if (!conn) { drmModeFreeResources(res); return; }
	target_conn = conn->connector_id;
	target_mode = conn->modes[0];
	for (int m = 0; m < conn->count_modes; m++)
		if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) { target_mode = conn->modes[m]; break; }
	have_mode = true;
	target_crtc = conn->encoder_id ? 0 : 0;
	drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(drm_fd, conn->encoder_id) : NULL;
	if (enc) { target_crtc = enc->crtc_id; drmModeFreeEncoder(enc); }
	for (int i = 0; i < conn->count_encoders && !target_crtc; i++)
	{
		drmModeEncoder *e = drmModeGetEncoder(drm_fd, conn->encoders[i]);
		if (!e) continue;
		for (int k = 0; k < res->count_crtcs && !target_crtc; k++)
			if (e->possible_crtcs & (1u << k)) target_crtc = res->crtcs[k];
		drmModeFreeEncoder(e);
	}
	if (!target_crtc && res->count_crtcs) target_crtc = res->crtcs[0];
	for (int k = 0; k < res->count_crtcs; k++) if (res->crtcs[k] == target_crtc) target_crtc_index = k;
	if (have_mode) drmModeCreatePropertyBlob(drm_fd, &target_mode, sizeof target_mode, &mode_blob);

	drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd);
	if (!pr) { drmModeFreeConnector(conn); drmModeFreeResources(res); return; }
	for (uint32_t i = 0; i < pr->count_planes; i++)
	{
		drmModePlane *p = drmModeGetPlane(drm_fd, pr->planes[i]);
		if (!p) continue;
		if (!(p->possible_crtcs & (1u << target_crtc_index))) { drmModeFreePlane(p); continue; }
		uint64_t type = 0;
		drmModeObjectProperties *op = drmModeObjectGetProperties(drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		if (op)
		{
			for (uint32_t k = 0; k < op->count_props; k++)
			{
				drmModePropertyPtr prp = drmModeGetProperty(drm_fd, op->props[k]);
				if (prp && !strcmp(prp->name, "type")) type = op->prop_values[k];
				if (prp) drmModeFreeProperty(prp);
			}
			drmModeFreeObjectProperties(op);
		}
		if (type == DRM_PLANE_TYPE_PRIMARY && !primary.id)
		{
			primary.id = p->plane_id;
			primary.crtc_id = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
			primary.fb_id = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			primary.src_x = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "SRC_X");
			primary.src_y = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
			primary.src_w = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "SRC_W");
			primary.src_h = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "SRC_H");
			primary.crt_x = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
			primary.crt_y = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
			primary.crt_w = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
			primary.crt_h = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
			primary.rot_id = prop_id(primary.id, DRM_MODE_OBJECT_PLANE, "rotation");
		}
		else if (type == DRM_PLANE_TYPE_OVERLAY && !overlay.id)
		{
			overlay.id = p->plane_id;
			overlay.crtc_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
			overlay.fb_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			overlay.src_x = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "SRC_X");
			overlay.src_y = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
			overlay.src_w = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "SRC_W");
			overlay.src_h = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "SRC_H");
			overlay.crt_x = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
			overlay.crt_y = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
			overlay.crt_w = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
			overlay.crt_h = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
			overlay.zpos_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "zpos");
			overlay.alpha_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "alpha");
			overlay.blend_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "pixel blend mode");
			overlay.rot_id = prop_id(overlay.id, DRM_MODE_OBJECT_PLANE, "rotation");
		}
		drmModeFreePlane(p);
	}
	have_overlay = overlay.id != 0;
	drmModeFreePlaneResources(pr);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
}

static bool make_overlay_fb(void)
{
	struct drm_mode_create_dumb c = { 0 };
	c.width = overlay_w; c.height = overlay_h; c.bpp = 32;
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
	uint32_t h[4] = { c.handle }, p[4] = { c.pitch }, o[4] = { 0 };
	uint64_t m[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };
	if (drmModeAddFB2WithModifiers(drm_fd, overlay_w, overlay_h, DRM_FORMAT_ARGB8888, h, p, o, m, &overlay_fb, DRM_MODE_FB_MODIFIERS))
	{
		struct drm_mode_destroy_dumb d = { 0 }; d.handle = c.handle; drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		return false;
	}
	struct drm_mode_map_dumb mp = { 0 };
	mp.handle = c.handle;
	if (!drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mp))
	{
		uint32_t *px = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mp.offset);
		if (px != MAP_FAILED)
		{
			for (uint32_t y = 0; y < overlay_h; y++)
				for (uint32_t x = 0; x < overlay_w; x++)
					px[y * (c.pitch / 4) + x] = (x < overlay_w / 2 ? 0xff20c0ffu : 0xff2040ffu);
			munmap(px, c.size);
		}
	}
	return true;
}

static bool import_buffer(struct buffer *b)
{
	if (b->imported || b->import_failed) return b->imported;
	uint32_t handles[MAX_PLANES] = { 0 };
	for (int i = 0; i < b->nplanes; i++)
		if (drmPrimeFDToHandle(drm_fd, b->fd[i], &handles[i]))
		{
			printf("IMPORT fail plane=%d errno=%d (%s)\n", i, errno, strerror(errno));
			b->import_failed = true;
			return false;
		}
	uint64_t mods[MAX_PLANES];
	for (int i = 0; i < MAX_PLANES; i++) mods[i] = b->modifier;
	if (drmModeAddFB2WithModifiers(drm_fd, b->width, b->height, b->format, handles, b->stride, b->offset, mods, &b->fb, DRM_MODE_FB_MODIFIERS))
	{
		int e1 = errno;
		printf("IMPORT modifier fail errno=%d (%s) format=%s mod=0x%016" PRIx64 " %ux%u stride=%u\n",
		       e1, strerror(e1), fcc(b->format, (char[16]){0}), b->modifier, b->width, b->height, b->stride[0]);
		uint32_t f2 = 0;
		if (drmModeAddFB2(drm_fd, b->width, b->height, b->format, handles, b->stride, b->offset, &f2, 0) == 0)
		{
			printf("IMPORT legacy-addfb2 ok (buffer treated linear; diagnostic only)\n");
			drmModeRmFB(drm_fd, f2);
		}
		else printf("IMPORT legacy-addfb2 fail errno=%d (%s)\n", errno, strerror(errno));
		uint64_t lm[MAX_PLANES];
		for (int i = 0; i < MAX_PLANES; i++) lm[i] = DRM_FORMAT_MOD_LINEAR;
		uint32_t f3 = 0;
		if (drmModeAddFB2WithModifiers(drm_fd, b->width, b->height, b->format, handles, b->stride, b->offset, lm, &f3, DRM_MODE_FB_MODIFIERS) == 0)
		{
			printf("IMPORT declared-linear ok (diagnostic only)\n");
			drmModeRmFB(drm_fd, f3);
		}
		else printf("IMPORT declared-linear fail errno=%d (%s)\n", errno, strerror(errno));
		b->import_failed = true;
		return false;
	}
	b->imported = true;
	return true;
}

static void addp(drmModeAtomicReq *r, uint32_t obj, uint32_t prop, uint64_t v)
{
	if (prop) drmModeAtomicAddProperty(r, obj, prop, v);
}

static void plane_req(drmModeAtomicReq *r, const struct plane *p, uint32_t fb, uint32_t sw, uint32_t sh,
                      uint64_t dw, uint64_t dh, uint64_t rotation)
{
	addp(r, p->id, p->crtc_id, target_crtc);
	addp(r, p->id, p->fb_id, fb);
	addp(r, p->id, p->src_x, 0);
	addp(r, p->id, p->src_y, 0);
	addp(r, p->id, p->src_w, (uint64_t)sw << 16);
	addp(r, p->id, p->src_h, (uint64_t)sh << 16);
	addp(r, p->id, p->crt_x, 0);
	addp(r, p->id, p->crt_y, 0);
	addp(r, p->id, p->crt_w, dw);
	addp(r, p->id, p->crt_h, dh);
	if (p->rot_id) addp(r, p->id, p->rot_id, rotation);
}

static int commit_test(drmModeAtomicReq *r)
{
	if (!r) return ENOMEM;
	errno = 0;
	int ret = drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	int err = errno;
	drmModeAtomicFree(r);
	return ret ? (err ? err : EINVAL) : 0;
}

static drmModeAtomicReq *base_req(void)
{
	drmModeAtomicReq *r = drmModeAtomicAlloc();
	addp(r, target_conn, prop_id(target_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), target_crtc);
	addp(r, target_crtc, prop_id(target_crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), mode_blob);
	addp(r, target_crtc, prop_id(target_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
	return r;
}

static void test_buffer(struct buffer *b)
{
	printf("TEST client=%s size=%ux%u format=%s modifier=0x%016" PRIx64 " planes=%d\n",
	       report_client, b->width, b->height, fcc(b->format, (char[16]){0}), b->modifier, b->nplanes);
	if (!use_drm) return;
	if (!import_buffer(b)) { printf("TEST skipped: the buffer could not be imported\n"); return; }
	uint32_t dw = target_mode.hdisplay, dh = target_mode.vdisplay;
	drmModeAtomicReq *r = base_req();
	plane_req(r, &primary, b->fb, b->width, b->height, dw, dh, DRM_MODE_ROTATE_0);
	int e = commit_test(r);
	printf("TEST primary %s", e ? "reject" : "ok");
	if (e) printf(" errno=%d (%s)", e, strerror(e));
	printf("\n");
	if (have_overlay && overlay_fb)
	{
		r = base_req();
		plane_req(r, &primary, b->fb, b->width, b->height, dw, dh, DRM_MODE_ROTATE_0);
		plane_req(r, &overlay, overlay_fb, overlay_w, overlay_h, overlay_w, overlay_h, DRM_MODE_ROTATE_0);
		if (overlay.zpos_id) addp(r, overlay.id, overlay.zpos_id, 255);
		if (overlay.alpha_id) addp(r, overlay.id, overlay.alpha_id, 0xffff);
		if (overlay.blend_id) addp(r, overlay.id, overlay.blend_id, 0);
		e = commit_test(r);
		printf("TEST overlay %s", e ? "reject" : "ok");
		if (e) printf(" errno=%d (%s)", e, strerror(e));
		printf("\n");
	}
	if (primary.rot_id && b->width > b->height && target_mode.vdisplay > target_mode.hdisplay)
	{
		r = base_req();
		plane_req(r, &primary, b->fb, b->width, b->height, target_mode.vdisplay, target_mode.hdisplay, DRM_MODE_ROTATE_90);
		e = commit_test(r);
		rotation_tested = true;
		rotation_ok = e == 0;
		printf("TEST rotate90 %s", e ? "reject" : "ok");
		if (e) printf(" errno=%d (%s)", e, strerror(e));
		printf("\n");
	}
	printf("TEST import ok fb=%u\n", b->fb);
}

static void present(struct buffer *b);
static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	(void)fd; (void)seq; (void)sec; (void)usec; (void)data;
	if (current_buf && current_buf->res) wl_buffer_send_release(current_buf->res);
	current_buf = NULL;
	flip_pending = false;
	frames_live++;
	if (the_surface && the_surface->frame_cb)
	{
		wl_callback_send_done(the_surface->frame_cb, now_ms());
		wl_resource_destroy(the_surface->frame_cb);
		the_surface->frame_cb = NULL;
	}
	if (pending_buf) { struct buffer *b = pending_buf; pending_buf = NULL; present(b); }
}

static void present(struct buffer *b)
{
	if (!do_live || !b || !b->imported) return;
	if (flip_pending) { if (pending_buf && pending_buf != b && pending_buf->res) wl_buffer_send_release(pending_buf->res); pending_buf = b; return; }
	drmModeAtomicReq *r = base_req();
	uint64_t rot = DRM_MODE_ROTATE_0;
	uint32_t sw = b->width, sh = b->height, dw = b->width, dh = b->height;
	if (rotation_ok) { rot = DRM_MODE_ROTATE_90; dw = target_mode.hdisplay; dh = target_mode.vdisplay; }
	else { dw = target_mode.hdisplay; dh = target_mode.vdisplay; }
	plane_req(r, &primary, b->fb, sw, sh, dw, dh, rot);
	if (overlay_on && have_overlay && overlay_fb)
	{
		plane_req(r, &overlay, overlay_fb, overlay_w, overlay_h, overlay_w, overlay_h, DRM_MODE_ROTATE_0);
		if (overlay.zpos_id) addp(r, overlay.id, overlay.zpos_id, 255);
		if (overlay.alpha_id) addp(r, overlay.id, overlay.alpha_id, 0xffff);
		if (overlay.blend_id) addp(r, overlay.id, overlay.blend_id, 0);
	}
	errno = 0;
	if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_PAGE_FLIP_EVENT, NULL))
		printf("LIVE flip fail errno=%d (%s)\n", errno, strerror(errno));
	else
	{
		flip_pending = true;
		current_buf = b;
		if (!live_started) { live_started = true; printf("LIVE started\n"); }
	}
	drmModeAtomicFree(r);
}

// ---- Wayland protocol ----------------------------------------------------------------------------------------------

static void buffer_destroy(struct wl_client *c, struct wl_resource *res)
{
	(void)c;
	struct buffer *b = wl_resource_get_user_data(res);
	if (!b) return;
	for (int i = 0; i < b->nplanes; i++) if (b->fd[i] >= 0) close(b->fd[i]);
	if (b->fb && drm_fd >= 0) drmModeRmFB(drm_fd, b->fb);
	wl_list_remove(&b->link);
	free(b);
	wl_resource_destroy(res);
}

static const struct wl_buffer_interface buffer_impl = { .destroy = buffer_destroy };

static void capture(struct buffer *b)
{
	if (b->shm)
	{
		buf_count++;
		printf("BUFFER client=%s type=shm size=%ux%u format=%s stride=%u (not a DMA-BUF; no DRM test)\n",
		       report_client, b->width, b->height, fcc(b->format, (char[16]){0}), b->stride[0]);
		if (b->res) wl_buffer_send_release(b->res);
		if (the_surface && the_surface->frame_cb)
		{
			wl_callback_send_done(the_surface->frame_cb, now_ms());
			wl_resource_destroy(the_surface->frame_cb);
			the_surface->frame_cb = NULL;
		}
		return;
	}
	buf_count++;
	if (buf_count == 1 || b->format != last_fmt || b->modifier != last_mod || b->width != last_w || b->height != last_h || b->nplanes != last_planes)
	{
		buf_unique++;
		last_fmt = b->format; last_mod = b->modifier; last_w = b->width; last_h = b->height; last_planes = b->nplanes;
		printf("BUFFER client=%s type=dmabuf format=%s modifier=0x%016" PRIx64 " (%s) size=%ux%u planes=%d",
		       report_client, fcc(b->format, (char[16]){0}), b->modifier, mod_name(b->modifier), b->width, b->height, b->nplanes);
		for (int i = 0; i < b->nplanes; i++) printf(" p%d=off%u/str%u", i, b->offset[i], b->stride[i]);
		printf("\n");
	}
	if ((do_test || do_live) && !tested) { tested = true; test_buffer(b); }
	if (!do_live)
	{
		if (b->res) wl_buffer_send_release(b->res);
		if (the_surface && the_surface->frame_cb)
		{
			wl_callback_send_done(the_surface->frame_cb, now_ms());
			wl_resource_destroy(the_surface->frame_cb);
			the_surface->frame_cb = NULL;
		}
	}
	else present(b);
}

static void surf_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void surf_attach(struct wl_client *c, struct wl_resource *res, struct wl_resource *buf, int32_t x, int32_t y)
{
	(void)c; (void)x; (void)y;
	struct surface *s = wl_resource_get_user_data(res);
	s->pending = buf ? wl_resource_get_user_data(buf) : NULL;
}
static void surf_damage(struct wl_client *c, struct wl_resource *res, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)res;(void)x;(void)y;(void)w;(void)h; }
static void surf_frame(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct surface *s = wl_resource_get_user_data(res);
	struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, wl_resource_get_version(res), id);
	s->frame_cb = cb;
}
static void surf_region(struct wl_client *c, struct wl_resource *res, struct wl_resource *r) { (void)c;(void)res;(void)r; }
static void surf_commit(struct wl_client *c, struct wl_resource *res)
{
	struct surface *s = wl_resource_get_user_data(res);
	if (!s->configured && s->toplevel)
	{
		struct wl_array states;
		wl_array_init(&states);
		uint32_t *st = wl_array_add(&states, sizeof *st);
		*st = XDG_TOPLEVEL_STATE_ACTIVATED;
		xdg_toplevel_send_configure(s->toplevel, 1240, 1080, &states);
		xdg_surface_send_configure(s->xdg, wl_display_next_serial(display));
		wl_array_release(&states);
		s->configured = true;
		return;
	}
	if (s->pending) { s->current = s->pending; s->pending = NULL; capture(s->current); }
}
static void surf_transform(struct wl_client *c, struct wl_resource *res, int32_t t) { (void)c;(void)res;(void)t; }
static void surf_scale(struct wl_client *c, struct wl_resource *res, int32_t s) { (void)c;(void)res;(void)s; }
static void surf_damage_buffer(struct wl_client *c, struct wl_resource *res, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)res;(void)x;(void)y;(void)w;(void)h; }
static const struct wl_surface_interface surface_impl = {
	.destroy = surf_destroy, .attach = surf_attach, .damage = surf_damage, .frame = surf_frame,
	.set_opaque_region = surf_region, .set_input_region = surf_region, .commit = surf_commit,
	.set_buffer_transform = surf_transform, .set_buffer_scale = surf_scale, .damage_buffer = surf_damage_buffer,
};

static void region_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void region_add(struct wl_client *c, struct wl_resource *res, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)res;(void)x;(void)y;(void)w;(void)h; }
static void region_sub(struct wl_client *c, struct wl_resource *res, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)res;(void)x;(void)y;(void)w;(void)h; }
static const struct wl_region_interface region_impl = { .destroy = region_destroy, .add = region_add, .subtract = region_sub };

static void comp_surface(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct surface *s = calloc(1, sizeof *s);
	s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(s->res, &surface_impl, s, NULL);
	the_surface = s;
}
static void comp_region(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = { .create_surface = comp_surface, .create_region = comp_region };

static void top_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void top_noop2(struct wl_client *c, struct wl_resource *res, struct wl_resource *o) { (void)c;(void)res;(void)o; }
static void top_noop_str(struct wl_client *c, struct wl_resource *res, const char *s) { (void)c;(void)res;(void)s; }
static void top_noop_seat(struct wl_client *c, struct wl_resource *res, struct wl_resource *s, uint32_t serial, int32_t x, int32_t y) { (void)c;(void)res;(void)s;(void)serial;(void)x;(void)y; }
static void top_move(struct wl_client *c, struct wl_resource *res, struct wl_resource *s, uint32_t serial) { (void)c;(void)res;(void)s;(void)serial; }
static void top_resize(struct wl_client *c, struct wl_resource *res, struct wl_resource *s, uint32_t serial, uint32_t edges) { (void)c;(void)res;(void)s;(void)serial;(void)edges; }
static void top_2i(struct wl_client *c, struct wl_resource *res, int32_t w, int32_t h) { (void)c;(void)res;(void)w;(void)h; }
static void top_noop0(struct wl_client *c, struct wl_resource *res) { (void)c;(void)res; }
static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = top_destroy, .set_parent = top_noop2, .set_title = top_noop_str, .set_app_id = top_noop_str,
	.show_window_menu = top_noop_seat, .move = top_move, .resize = top_resize,
	.set_max_size = top_2i, .set_min_size = top_2i,
	.set_maximized = top_noop0, .unset_maximized = top_noop0,
	.set_fullscreen = top_noop2, .unset_fullscreen = top_noop0, .set_minimized = top_noop0,
};

static void ptr_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surf, int32_t x, int32_t y) { (void)c;(void)r;(void)serial;(void)surf;(void)x;(void)y; }
static void ptr_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { .set_cursor = ptr_cursor, .release = ptr_release };
static void kb_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl = { .release = kb_release };
static void touch_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = { .release = touch_release };

static void xdg_surf_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void xdg_surf_toplevel(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct surface *s = wl_resource_get_user_data(res);
	s->toplevel = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(s->toplevel, &toplevel_impl, s, NULL);
}
static void xdg_surf_popup(struct wl_client *c, struct wl_resource *res, uint32_t id, struct wl_resource *parent, struct wl_resource *pos) { (void)c;(void)res;(void)id;(void)parent;(void)pos; }
static void xdg_surf_geom(struct wl_client *c, struct wl_resource *res, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)res;(void)x;(void)y;(void)w;(void)h; }
static void xdg_surf_ack(struct wl_client *c, struct wl_resource *res, uint32_t serial) { (void)c;(void)res;(void)serial; }
static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xdg_surf_destroy, .get_toplevel = xdg_surf_toplevel, .get_popup = xdg_surf_popup,
	.set_window_geometry = xdg_surf_geom, .ack_configure = xdg_surf_ack,
};

static void seat_get_ptr(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(r, &pointer_impl, NULL, NULL);
}
static void seat_get_kb(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(r, &keyboard_impl, NULL, NULL);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(r, &touch_impl, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static const struct wl_seat_interface seat_impl = { .get_pointer = seat_get_ptr, .get_keyboard = seat_get_kb, .get_touch = seat_get_touch, .release = seat_release };

static void output_release(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static const struct wl_output_interface output_impl = { .release = output_release };

static void drm_authenticate(struct wl_client *c, struct wl_resource *res, uint32_t magic)
{
	(void)c;
	if (drm_fd >= 0 && drmAuthMagic(drm_fd, magic) == 0) { wl_drm_send_authenticated(res); printf("WL_DRM authenticated\n"); }
	else { printf("WL_DRM auth fail errno=%d (%s)\n", errno, strerror(errno)); wl_resource_post_error(res, 0, "authenticate failed"); }
}
static void drm_create_buffer(struct wl_client *c, struct wl_resource *res, uint32_t id, uint32_t name, int32_t w, int32_t h, uint32_t stride, uint32_t format)
{
	(void)c; (void)name; (void)w; (void)h; (void)stride; (void)format;
	printf("WL_DRM create_buffer id=%u (GEM name path; dmabuf expected)\n", id);
	wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
}
static void drm_create_planar(struct wl_client *c, struct wl_resource *res, uint32_t id, uint32_t name, int32_t w, int32_t h, uint32_t format, int32_t o0, int32_t s0, int32_t o1, int32_t s1, int32_t o2, int32_t s2)
{
	(void)c; (void)id; (void)name; (void)w; (void)h; (void)format; (void)o0; (void)s0; (void)o1; (void)s1; (void)o2; (void)s2;
	wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
}
static void drm_create_prime(struct wl_client *c, struct wl_resource *res, uint32_t id, int32_t fd, int32_t w, int32_t h, uint32_t format, int32_t o0, int32_t s0, int32_t o1, int32_t s1, int32_t o2, int32_t s2)
{
	(void)c; (void)id; (void)fd; (void)w; (void)h; (void)format; (void)o0; (void)s0; (void)o1; (void)s1; (void)o2; (void)s2;
	wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
}
static const struct wl_drm_interface drm_impl = {
	.authenticate = drm_authenticate, .create_buffer = drm_create_buffer,
	.create_planar_buffer = drm_create_planar, .create_prime_buffer = drm_create_prime,
};
static void drm_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *res = wl_resource_create(c, &wl_drm_interface, version < 2 ? version : 2, id);
	wl_resource_set_implementation(res, &drm_impl, NULL, NULL);
	wl_drm_send_device(res, drm_name);
	wl_drm_send_format(res, DRM_FORMAT_ARGB8888);
	wl_drm_send_format(res, DRM_FORMAT_XRGB8888);
	wl_drm_send_capabilities(res, WL_DRM_CAPABILITY_PRIME);
	printf("WL_DRM device=%s\n", drm_name);
}

struct shm_pool { int fd; size_t size; };
static void shm_pool_destroy(struct wl_client *c, struct wl_resource *res)
{
	(void)c;
	struct shm_pool *p = wl_resource_get_user_data(res);
	if (p) { if (p->fd >= 0) close(p->fd); free(p); }
	wl_resource_destroy(res);
}
static void shm_pool_resize(struct wl_client *c, struct wl_resource *res, int32_t size) { (void)c;(void)res;(void)size; }
static void shm_pool_create_buffer(struct wl_client *c, struct wl_resource *res, uint32_t id, int32_t off, int32_t w, int32_t h, int32_t stride, uint32_t format)
{
	(void)off;
	struct buffer *b = calloc(1, sizeof *b);
	b->shm = true;
	b->width = w; b->height = h; b->format = format; b->stride[0] = stride; b->nplanes = 1;
	b->res = wl_resource_create(c, &wl_buffer_interface, 1, id);
	wl_resource_set_implementation(b->res, &buffer_impl, b, NULL);
	wl_list_insert(&buffers, &b->link);
	(void)res;
}
static const struct wl_shm_pool_interface shm_pool_impl = {
	.create_buffer = shm_pool_create_buffer, .destroy = shm_pool_destroy, .resize = shm_pool_resize,
};
static void shm_create_pool(struct wl_client *c, struct wl_resource *res, uint32_t id, int32_t fd, int32_t size)
{
	struct shm_pool *p = calloc(1, sizeof *p);
	p->fd = dup(fd);
	p->size = size > 0 ? (size_t)size : 0;
	struct wl_resource *r = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
	wl_resource_set_implementation(r, &shm_pool_impl, p, NULL);
	(void)res;
}
static const struct wl_shm_interface shm_impl = { .create_pool = shm_create_pool };
static void shm_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_shm_interface, version < 1 ? version : 1, id);
	wl_resource_set_implementation(r, &shm_impl, NULL, NULL);
	wl_shm_send_format(r, DRM_FORMAT_ARGB8888);
	wl_shm_send_format(r, DRM_FORMAT_XRGB8888);
}

static void seat_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, version < 5 ? version : 5, id);
	wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(r, 0);
	wl_seat_send_name(r, "seat0");
}
static void output_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_output_interface, version < 3 ? version : 3, id);
	wl_resource_set_implementation(r, &output_impl, NULL, NULL);
	wl_output_send_geometry(r, 0, 0, 340, 400, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Banana", "Retroid Pocket Classic", WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1240, 1080, 60000);
	if (version >= 2) wl_output_send_scale(r, 1);
	if (version >= 2) wl_output_send_done(r);
}

static void xdg_wm_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void pos_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void pos_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void pos_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c;(void)r;(void)a; }
static void pos_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) { (void)c;(void)r;(void)g; }
static void pos_adjust(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c;(void)r;(void)a; }
static void pos_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c;(void)r;(void)x;(void)y; }
static void pos_1(struct wl_client *c, struct wl_resource *r) { (void)c;(void)r; }
static void pos_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c;(void)r;(void)w;(void)h; }
static void pos_parent_cfg(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c;(void)r;(void)serial; }
static const struct xdg_positioner_interface positioner_impl = {
	.destroy = pos_destroy, .set_size = pos_size, .set_anchor_rect = pos_rect, .set_anchor = pos_anchor,
	.set_gravity = pos_gravity, .set_constraint_adjustment = pos_adjust, .set_offset = pos_offset,
	.set_reactive = pos_1, .set_parent_size = pos_parent_size, .set_parent_configure = pos_parent_cfg,
};
static void xdg_wm_positioner(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(r, &positioner_impl, NULL, NULL);
}
static void xdg_wm_get_surface(struct wl_client *c, struct wl_resource *res, uint32_t id, struct wl_resource *surface)
{
	struct surface *s = wl_resource_get_user_data(surface);
	s->xdg = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(s->xdg, &xdg_surface_impl, s, NULL);
}
static void xdg_wm_pong(struct wl_client *c, struct wl_resource *res, uint32_t serial) { (void)c;(void)res;(void)serial; }
static const struct xdg_wm_base_interface xdg_wm_impl = {
	.destroy = xdg_wm_destroy, .create_positioner = xdg_wm_positioner, .get_xdg_surface = xdg_wm_get_surface, .pong = xdg_wm_pong,
};


static void xdg_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, version < 6 ? version : 6, id);
	wl_resource_set_implementation(r, &xdg_wm_impl, NULL, NULL);
}
static void compositor_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version < 4 ? version : 4, id);
	wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

// ---- dmabuf --------------------------------------------------------------------------------------------------------

static void params_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void params_add(struct wl_client *c, struct wl_resource *res, int32_t fd, uint32_t plane, uint32_t off, uint32_t stride, uint32_t hi, uint32_t lo)
{
	(void)c;
	struct buffer *b = wl_resource_get_user_data(res);
	if (plane >= MAX_PLANES) { wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane idx"); return; }
	b->fd[plane] = dup(fd);
	b->offset[plane] = off;
	b->stride[plane] = stride;
	b->modifier = ((uint64_t)hi << 32) | lo;
	if ((int)plane + 1 > b->nplanes) b->nplanes = plane + 1;
}
static void params_create(struct wl_client *c, struct wl_resource *res, int32_t w, int32_t h, uint32_t format, uint32_t flags)
{
	struct buffer *b = wl_resource_get_user_data(res);
	b->width = w; b->height = h; b->format = format; b->flags = flags;
	b->res = wl_resource_create(c, &wl_buffer_interface, 1, 0);
	wl_resource_set_implementation(b->res, &buffer_impl, b, NULL);
	wl_list_insert(&buffers, &b->link);
	zwp_linux_buffer_params_v1_send_created(res, b->res);
}
static void params_create_immed(struct wl_client *c, struct wl_resource *res, uint32_t id, int32_t w, int32_t h, uint32_t format, uint32_t flags)
{
	struct buffer *b = wl_resource_get_user_data(res);
	b->width = w; b->height = h; b->format = format; b->flags = flags;
	b->res = wl_resource_create(c, &wl_buffer_interface, 1, id);
	wl_resource_set_implementation(b->res, &buffer_impl, b, NULL);
	wl_list_insert(&buffers, &b->link);
}
static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = params_destroy, .add = params_add, .create = params_create, .create_immed = params_create_immed,
};

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static void feedback_destroy(struct wl_client *c, struct wl_resource *res) { (void)c; wl_resource_destroy(res); }
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = { .destroy = feedback_destroy };

// linux-dmabuf v4 feedback: the client (Mesa) gets the DRM device and the exact (format, modifier) table from it.
static void feedback_send(struct wl_client *c, uint32_t id, uint32_t version)
{
	struct wl_resource *res = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
	wl_resource_set_implementation(res, &feedback_impl, NULL, NULL);
	int fd = memfd_create("dmabuf-format-table", MFD_CLOEXEC);
	if (fd >= 0)
	{
		size_t sz = (size_t)nadv * sizeof(struct { uint32_t format, padding; uint64_t modifier; });
		struct { uint32_t format, padding; uint64_t modifier; } *t = malloc(sz);
		for (int i = 0; i < nadv; i++) { t[i].format = adv[i].format; t[i].padding = 0; t[i].modifier = adv[i].modifier; }
		if (write(fd, t, sz) == (ssize_t)sz) zwp_linux_dmabuf_feedback_v1_send_format_table(res, fd, (uint32_t)sz);
		free(t);
		close(fd);
	}
	struct wl_array dev;
	wl_array_init(&dev);
	void *p = wl_array_add(&dev, sizeof target_dev);
	if (p) memcpy(p, &target_dev, sizeof target_dev);
	zwp_linux_dmabuf_feedback_v1_send_main_device(res, &dev);
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(res, &dev);
	wl_array_release(&dev);
	uint16_t *idx = NULL;
	struct wl_array indices;
	wl_array_init(&indices);
	for (int i = 0; i < nadv; i++) { idx = wl_array_add(&indices, sizeof *idx); if (idx) *idx = (uint16_t)i; }
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(res, &indices);
	wl_array_release(&indices);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(res, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(res);
	zwp_linux_dmabuf_feedback_v1_send_done(res);
	(void)version;
}

static void dmabuf_create_params(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct buffer *b = calloc(1, sizeof *b);
	for (int i = 0; i < MAX_PLANES; i++) b->fd[i] = -1;
	b->res = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(res), id);
	wl_resource_set_implementation(b->res, &params_impl, b, NULL);
}
static void dmabuf_default_feedback(struct wl_client *c, struct wl_resource *res, uint32_t id) { (void)res; feedback_send(c, id, 4); }
static void dmabuf_surface_feedback(struct wl_client *c, struct wl_resource *res, uint32_t id, struct wl_resource *surface) { (void)res; (void)surface; feedback_send(c, id, 4); }
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy, .create_params = dmabuf_create_params,
	.get_default_feedback = dmabuf_default_feedback, .get_surface_feedback = dmabuf_surface_feedback,
};


static void read_adv(void)
{
	// advertise what the primary plane lists for the formats a client would use; fall back to the report's pairs
	if (drm_fd < 0 || !primary.id)
	{
		adv[nadv++] = (struct adv){ DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR };
		adv[nadv++] = (struct adv){ DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR };
		adv[nadv++] = (struct adv){ DRM_FORMAT_ARGB8888, 0x0500000000000001ULL };
		adv[nadv++] = (struct adv){ DRM_FORMAT_XRGB8888, 0x0500000000000001ULL };
		return;
	}
	drmModeObjectProperties *op = drmModeObjectGetProperties(drm_fd, primary.id, DRM_MODE_OBJECT_PLANE);
	if (!op) return;
	for (uint32_t i = 0; i < op->count_props; i++)
	{
		drmModePropertyPtr p = drmModeGetProperty(drm_fd, op->props[i]);
		if (p && !strcmp(p->name, "IN_FORMATS"))
		{
			drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(drm_fd, (uint32_t)op->prop_values[i]);
			if (blob && blob->data && blob->length >= sizeof(struct drm_format_modifier_blob))
			{
				const struct drm_format_modifier_blob *b = blob->data;
				const uint32_t *fmts = (const uint32_t *)((const char *)b + b->formats_offset);
				const struct drm_format_modifier *m = (const struct drm_format_modifier *)((const char *)b + b->modifiers_offset);
				for (uint32_t k = 0; k < b->count_modifiers; k++)
				{
					for (uint32_t bit = 0; bit < 64; bit++)
					{
						if (!(m[k].formats & (1ull << bit))) continue;
						uint32_t at = m[k].offset + bit;
						if (at >= b->count_formats) continue;
						if (fmts[at] != DRM_FORMAT_ARGB8888 && fmts[at] != DRM_FORMAT_XRGB8888) continue;
						if (opt_linear && m[k].modifier != DRM_FORMAT_MOD_LINEAR) continue;
						if (nadv < MAX_ADV) adv[nadv++] = (struct adv){ fmts[at], m[k].modifier };
					}
				}
			}
			if (blob) drmModeFreePropertyBlob(blob);
		}
		if (p) drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(op);
	if (!nadv)
	{
		adv[nadv++] = (struct adv){ DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR };
		adv[nadv++] = (struct adv){ DRM_FORMAT_ARGB8888, 0x0500000000000001ULL };
	}
}

static void dmabuf_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	(void)data;
	uint32_t v = version < 4 ? version : 4;
	struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, v, id);
	wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
	for (int i = 0; i < nadv && v < 4; i++)
	{
		if (v < 3) { zwp_linux_dmabuf_v1_send_format(r, adv[i].format); continue; }
		bool seen = false;
		for (int k = 0; k < i; k++) if (adv[k].format == adv[i].format) seen = true;
		if (!seen) zwp_linux_dmabuf_v1_send_format(r, adv[i].format);
		zwp_linux_dmabuf_v1_send_modifier(r, adv[i].format, (uint32_t)(adv[i].modifier >> 32), (uint32_t)adv[i].modifier);
	}
	printf("GLOBAL dmabuf formats=%d\n", nadv);
}

// ---- main ----------------------------------------------------------------------------------------------------------

static int on_signal(int sig, void *data) { (void)sig; (void)data; if (display) wl_display_terminate(display); return 0; }
static int on_timer(void *data) { (void)data; wl_display_terminate(display); return 0; }
static int on_drm(int fd, uint32_t mask, void *data) { (void)fd; (void)mask; (void)data; drmEventContext ev = { .version = 2, .page_flip_handler = flip_done }; drmHandleEvent(drm_fd, &ev); return 0; }

static void usage(void)
{
	fprintf(stderr, "usage: dmabuf-capture [--socket NAME] [--seconds N] [--card PATH] [--drm] [--test] [--live] [--linear]"
	                " [--client CMD [args...]]\n");
}

int main(int argc, char **argv)
{
	char card[64] = "";
	char **client = NULL;
	int client_argc = 0;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_name = argv[++i];
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) run_seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--card") && i + 1 < argc) snprintf(card, sizeof card, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--drm")) use_drm = true;
		else if (!strcmp(argv[i], "--test")) { do_test = true; use_drm = true; }
		else if (!strcmp(argv[i], "--live")) { do_live = true; use_drm = true; do_test = true; }
		else if (!strcmp(argv[i], "--linear")) opt_linear = true;
		else if (!strcmp(argv[i], "--client")) { client = &argv[i + 1]; client_argc = argc - i - 1; break; }
		else { usage(); return 1; }
	}
	(void)client_argc;
	if (client)
	{
		const char *b = strrchr(client[0], '/');
		report_client = b ? b + 1 : client[0];
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	wl_list_init(&buffers);
	signal(SIGPIPE, SIG_IGN);

	if (use_drm)
	{
		const char *path = card[0] ? card : find_card();
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); return 1; }
		if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) || drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1))
		{ fprintf(stderr, "no atomic/universal planes on %s\n", path); return 1; }
		drmVersionPtr ver = drmGetVersion(drm_fd);
		snprintf(drm_name, sizeof drm_name, "%s", ver ? ver->name : "msm");
		printf("capture-server v1\ndrm card=%s driver=%s %d.%d.%d\n", path, drm_name,
		       ver ? ver->version_major : 0, ver ? ver->version_minor : 0, ver ? ver->version_patchlevel : 0);
		drmFreeVersion(ver);
		struct stat dst;
		if (!stat(path, &dst)) target_dev = dst.st_rdev;
		drm_target();
		if (!have_mode || !primary.id) { fprintf(stderr, "no display/primary plane\n"); return 1; }
		printf("display connector=%u crtc=%u mode=%ux%u primary=%u overlay=%u\n", target_conn, target_crtc,
		       target_mode.hdisplay, target_mode.vdisplay, primary.id, overlay.id);
		/* wl_drm authenticate needs master even for capture-only; the wrapper stops the menu first. */
		if (drmSetMaster(drm_fd)) printf("MASTER fail errno=%d (%s); wl_drm auth and TEST/LIVE may fail\n", errno, strerror(errno));
		else printf("MASTER ok\n");
		if (have_overlay) { make_overlay_fb(); overlay_on = do_live && overlay_fb; }
	}
	read_adv();

	display = wl_display_create();
	if (!display) { fprintf(stderr, "wl_display_create failed\n"); return 1; }
	loop = wl_display_get_event_loop(display);
	if (wl_display_add_socket(display, socket_name)) { fprintf(stderr, "add_socket %s failed\n", socket_name); return 1; }
	wl_global_create(display, &wl_compositor_interface, 4, NULL, compositor_bind);
	wl_global_create(display, &wl_output_interface, 3, NULL, output_bind);
	wl_global_create(display, &wl_shm_interface, 1, NULL, shm_bind);
	wl_global_create(display, &wl_seat_interface, 5, NULL, seat_bind);
	wl_global_create(display, &xdg_wm_base_interface, 6, NULL, xdg_bind);
	wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 4, NULL, dmabuf_bind);
	if (drm_fd >= 0) wl_global_create(display, &wl_drm_interface, 2, NULL, drm_bind);

	if (drm_fd >= 0) drm_src = wl_event_loop_add_fd(loop, drm_fd, WL_EVENT_READABLE, on_drm, NULL);
	timer = wl_event_loop_add_timer(loop, on_timer, NULL);
	wl_event_source_timer_update(timer, run_seconds * 1000);
	wl_event_loop_add_signal(loop, SIGTERM, on_signal, NULL);
	wl_event_loop_add_signal(loop, SIGINT, on_signal, NULL);

	printf("READY socket=%s seconds=%d drm=%d test=%d live=%d\n", socket_name, run_seconds, use_drm, do_test, do_live);

	if (client)
	{
		client_pid = fork();
		if (client_pid == 0)
		{
			setenv("WAYLAND_DISPLAY", socket_name, 1);
			execvp(client[0], client);
			_exit(127);
		}
		printf("CLIENT pid=%d cmd=%s\n", (int)client_pid, client[0]);
	}

	wl_display_run(display);

	if (client_pid > 0)
	{
		kill(client_pid, SIGTERM);
		bool gone = false;
		for (int i = 0; i < 20 && !gone; i++)
		{
			if (waitpid(client_pid, NULL, WNOHANG) == client_pid) { gone = true; break; }
			usleep(100000);
		}
		if (!gone)
		{
			printf("CLIENT did not stop after SIGTERM; sending SIGKILL\n");
			kill(client_pid, SIGKILL);
			waitpid(client_pid, NULL, 0);
		}
	}
	if (drm_fd >= 0)
	{
		if (mode_blob) drmModeDestroyPropertyBlob(drm_fd, mode_blob);
		if (overlay_fb) drmModeRmFB(drm_fd, overlay_fb);
		drmDropMaster(drm_fd);
		close(drm_fd);
	}
	wl_display_destroy(display);
	printf("SUMMARY client=%s buffers=%d unique=%d frames=%d live=%d rotation_tested=%d rotation_ok=%d\n",
	       report_client, buf_count, buf_unique, frames_live, live_started, rotation_tested, rotation_ok);
	return 0;
}
