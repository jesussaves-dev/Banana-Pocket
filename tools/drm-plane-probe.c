// drm-plane-probe: what the display controller can actually do -- planes, formats, modifiers, rotation,
// z-order, alpha, and which atomic combinations the driver accepts.
//
// It is a diagnostic for the "native DRM plane compositor" plan (docs/PLAN_DRM_COMPOSITOR.md). It never changes
// the display: the only atomic commits it makes are DRM_MODE_ATOMIC_TEST_ONLY, which run the driver's check
// without touching hardware. It is safe to run with the menu up for the enumeration; the atomic tests need DRM
// master, which the kernel requires for DRM_IOCTL_MODE_ATOMIC even when the commit is TEST_ONLY
// (drivers/gpu/drm/drm_ioctl.c: DRM_IOCTL_MODE_ATOMIC is registered DRM_MASTER).
//
// build (host, for a smoke test):
//   cc -std=c11 -O2 -Wall -Wextra tools/drm-plane-probe.c $(pkg-config --cflags --libs libdrm) -o /tmp/drm-plane-probe
// build (the handheld, with the image's own toolchain):
//   export SYS=output/retroid-pocket-classic/host/aarch64-buildroot-linux-gnu/sysroot
//   output/retroid-pocket-classic/host/bin/aarch64-linux-gcc -std=c11 -O2 -Wall -Wextra
//       --sysroot=$SYS -I$SYS/usr/include/libdrm tools/drm-plane-probe.c -ldrm -o /tmp/drm-plane-probe
//
// run:
//   drm-plane-probe                       enumerate (read-only; works while banana-ui runs)
//   drm-plane-probe --take-master         also run the TEST_ONLY matrix (needs an exclusive display)
//   drm-plane-probe --connector DSI-1 --card /dev/dri/card0
//
// Exit: 0 = enumeration and every test that could run completed; 2 = enumeration only (atomic tests skipped,
// usually because the display is held by another DRM master); 1 = fatal (cannot open the card or read resources).
//
// The exclusive window (dev unit): the menu must not hold master, and init respawns it. See
// docs/PLAN_DRM_COMPOSITOR.md "1.4 Running the atomic tests" for the bind-mount procedure. Do not run this on a
// device whose display you cannot afford to lose for a minute.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_PLANES 64
#define MAX_FORMATS 128
#define MAX_ENUMS 16

// These live in the kernel's drm_blend.h, not in the uapi headers: the ABI values are fixed.
enum { BLEND_PREMULTI = 0, BLEND_COVERAGE = 1, BLEND_PIXEL_NONE = 2 };

// ---- small helpers ------------------------------------------------------------------------------------------------

static const char *fourcc_name(uint32_t f, char out[16])
{
	out[0] = (char)(f & 0xff);
	out[1] = (char)((f >> 8) & 0xff);
	out[2] = (char)((f >> 16) & 0xff);
	out[3] = (char)((f >> 24) & 0xff);
	out[4] = 0;
	for (int i = 0; i < 4; i++)
		if (out[i] < 0x20 || out[i] > 0x7e)
		{
			snprintf(out, 16, "0x%08" PRIx32, f);
			break;
		}
	return out;
}

static const char *rotation_name(uint64_t v, char out[96])
{
	struct { uint64_t bit; const char *name; } bits[] = {
		{ DRM_MODE_ROTATE_0, "rotate-0" }, { DRM_MODE_ROTATE_90, "rotate-90" },
		{ DRM_MODE_ROTATE_180, "rotate-180" }, { DRM_MODE_ROTATE_270, "rotate-270" },
		{ DRM_MODE_REFLECT_X, "reflect-x" }, { DRM_MODE_REFLECT_Y, "reflect-y" },
	};
	out[0] = 0;
	if (v == 0) { snprintf(out, 96, "none"); return out; }
	for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++)
		if (v & bits[i].bit)
			snprintf(out + strlen(out), 96 - strlen(out), "%s%s", out[0] ? "|" : "", bits[i].name);
	return out;
}

static const char *plane_type_name(uint32_t t)
{
	switch (t)
	{
	case DRM_PLANE_TYPE_OVERLAY: return "OVERLAY";
	case DRM_PLANE_TYPE_PRIMARY: return "PRIMARY";
	case DRM_PLANE_TYPE_CURSOR: return "CURSOR";
	default: return "?";
	}
}

static const char *orientation_name(uint64_t v)
{
	switch (v)
	{
	case 1: return "upside-down";
	case 2: return "left-side-up";
	case 3: return "right-side-up";
	default: return "normal";
	}
}

static const char *conn_name(uint32_t type, uint32_t id, char out[32])
{
	const char *n = drmModeGetConnectorTypeName(type);
	snprintf(out, 32, "%s-%u", n ? n : "?", id);
	return out;
}

// ---- the card -----------------------------------------------------------------------------------------------------

static int fd = -1;
static bool opt_take_master;
static bool opt_no_atomic;
static const char *opt_card = "/dev/dri/card0";
static const char *opt_connector;

// ---- properties ---------------------------------------------------------------------------------------------------

// the property by name on an object: the current value in *val, the property itself returned (free with
// drmModeFreeProperty), or NULL. Works for connectors, CRTCs and planes.
static drmModePropertyPtr get_prop(uint32_t obj, uint32_t type, const char *name, uint64_t *val)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	if (!props) return NULL;
	drmModePropertyPtr found = NULL;
	for (uint32_t i = 0; i < props->count_props && !found; i++)
	{
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (!p) continue;
		if (!strcmp(p->name, name)) { if (val) *val = props->prop_values[i]; found = p; }
		else drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
	uint64_t v = 0;
	drmModePropertyPtr p = get_prop(obj, type, name, &v);
	if (!p) return 0;
	uint32_t id = p->prop_id;
	drmModeFreeProperty(p);
	return id;
}

// ---- planes -------------------------------------------------------------------------------------------------------

typedef struct
{
	uint32_t id, type, possible_crtcs;
	uint32_t formats[MAX_FORMATS];
	int nformats;

	// ids the atomic request needs
	uint32_t crtc_id, fb_id, src_x, src_y, src_w, src_h, crt_x, crt_y, crt_w, crt_h;
	uint32_t zpos_id, alpha_id, blend_id, rot_id;

	uint64_t zpos_min, zpos_max, zpos_value;
	bool has_zpos, zpos_immutable;
	bool has_alpha;
	uint64_t alpha_min, alpha_max;
	uint64_t blends[MAX_ENUMS];
	int nblends;
	uint64_t rotations[MAX_ENUMS];
	int nrotations;

	bool has_in_fence, has_in_formats, has_out_fence;
	bool linear_ar24, linear_xr24;	// a linear modifier is offered for that format (or there is no IN_FORMATS blob)
} Plane;

static Plane planes[MAX_PLANES];
static int nplanes;

static bool format_listed(const Plane *p, uint32_t fourcc)
{
	for (int i = 0; i < p->nformats; i++)
		if (p->formats[i] == fourcc) return true;
	return false;
}

// the IN_FORMATS blob: formats + the modifiers each is offered with. Linear (0) is what a dumb buffer gives us.
static void read_in_formats(Plane *p, drmModePropertyPtr prop, uint64_t val)
{
	if (!(prop->flags & DRM_MODE_PROP_BLOB)) return;
	drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, (uint32_t)val);
	if (!blob || !blob->data || blob->length < sizeof(struct drm_format_modifier_blob))
	{
		if (blob) drmModeFreePropertyBlob(blob);
		return;
	}
	const struct drm_format_modifier_blob *b = blob->data;
	if (b->formats_offset > blob->length || b->modifiers_offset > blob->length ||
	    (uint64_t)b->formats_offset + (uint64_t)b->count_formats * sizeof(uint32_t) > blob->length ||
	    (uint64_t)b->modifiers_offset + (uint64_t)b->count_modifiers * sizeof(struct drm_format_modifier) > blob->length)
	{
		drmModeFreePropertyBlob(blob);
		return;
	}
	const uint32_t *formats = (const uint32_t *)((const char *)b + b->formats_offset);
	const struct drm_format_modifier *mods =
		(const struct drm_format_modifier *)((const char *)b + b->modifiers_offset);
	for (uint32_t m = 0; m < b->count_modifiers; m++)
	{
		if (mods[m].modifier != DRM_FORMAT_MOD_LINEAR) continue;
		for (uint32_t bit = 0; bit < 64; bit++)
		{
			if (!(mods[m].formats & (1ull << bit))) continue;
			uint32_t at = mods[m].offset + bit;
			if (at >= b->count_formats) continue;
			if (formats[at] == DRM_FORMAT_ARGB8888) p->linear_ar24 = true;
			if (formats[at] == DRM_FORMAT_XRGB8888) p->linear_xr24 = true;
		}
	}
	drmModeFreePropertyBlob(blob);
}

static void read_plane(drmModePlane *pl)
{
	if (nplanes >= MAX_PLANES) return;
	Plane *p = &planes[nplanes++];
	memset(p, 0, sizeof *p);
	p->type = 0xff;	// unknown until the "type" property answers
	p->id = pl->plane_id;
	p->possible_crtcs = pl->possible_crtcs;
	for (uint32_t i = 0; i < pl->count_formats && p->nformats < MAX_FORMATS; i++)
		p->formats[p->nformats++] = pl->formats[i];

	uint64_t v = 0;
	drmModePropertyPtr t = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "type", &v);
	if (t)
	{
		p->type = (uint32_t)v;
		drmModeFreeProperty(t);
	}
	drmModePropertyPtr z = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "zpos", &v);
	if (z)
	{
		p->zpos_id = z->prop_id;
		p->has_zpos = true;
		p->zpos_value = v;
		p->zpos_immutable = (z->flags & DRM_MODE_PROP_IMMUTABLE) != 0;
		if (z->count_values >= 2) { p->zpos_min = z->values[0]; p->zpos_max = z->values[1]; }
		drmModeFreeProperty(z);
	}
	drmModePropertyPtr a = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "alpha", &v);
	if (a)
	{
		p->alpha_id = a->prop_id;
		p->has_alpha = true;
		if (a->count_values >= 2) { p->alpha_min = a->values[0]; p->alpha_max = a->values[1]; }
		drmModeFreeProperty(a);
	}
	drmModePropertyPtr bl = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &v);
	if (bl)
	{
		p->blend_id = bl->prop_id;
		for (int i = 0; i < bl->count_enums && p->nblends < MAX_ENUMS; i++)
			p->blends[p->nblends++] = bl->enums[i].value;
		drmModeFreeProperty(bl);
	}
	drmModePropertyPtr ro = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "rotation", &v);
	if (ro)
	{
		p->rot_id = ro->prop_id;
		for (int i = 0; i < ro->count_enums && p->nrotations < MAX_ENUMS; i++)
			p->rotations[p->nrotations++] = ro->enums[i].value;
		drmModeFreeProperty(ro);
	}
	drmModePropertyPtr inf = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &v);
	if (inf)
	{
		p->has_in_formats = true;
		read_in_formats(p, inf, v);
		drmModeFreeProperty(inf);
	}
	else
	{
		// a driver without IN_FORMATS takes a linear dumb buffer: assume linear (the TEST_ONLY run decides)
		p->linear_ar24 = p->linear_xr24 = true;
	}
	drmModePropertyPtr f = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", &v);
	if (f)
	{
		p->has_in_fence = true;
		drmModeFreeProperty(f);
	}
	f = get_prop(p->id, DRM_MODE_OBJECT_PLANE, "OUT_FENCE_PTR", &v);
	if (f)
	{
		p->has_out_fence = true;
		drmModeFreeProperty(f);
	}

	p->crtc_id = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
	p->fb_id = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "FB_ID");
	p->src_x = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "SRC_X");
	p->src_y = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
	p->src_w = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "SRC_W");
	p->src_h = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "SRC_H");
	p->crt_x = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "CRT_X");
	p->crt_y = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "CRT_Y");
	p->crt_w = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "CRT_W");
	p->crt_h = prop_id(p->id, DRM_MODE_OBJECT_PLANE, "CRT_H");
}

static bool supports_crtc(const Plane *p, uint32_t crtc_index)
{
	return (p->possible_crtcs & (1u << crtc_index)) != 0;
}

static bool rotation_supports(const Plane *p, uint64_t bit)
{
	for (int i = 0; i < p->nrotations; i++)
		if (p->rotations[i] & bit) return true;
	return false;
}

static bool blend_supports(const Plane *p, uint64_t value)
{
	for (int i = 0; i < p->nblends; i++)
		if (p->blends[i] == value) return true;
	return false;
}

// ---- dumb buffers -------------------------------------------------------------------------------------------------

typedef struct { uint32_t fb, handle, pitch; uint64_t size; } Buf;

static bool buf_new(uint32_t w, uint32_t h, uint32_t fourcc, Buf *b)
{
	memset(b, 0, sizeof *b);
	struct drm_mode_create_dumb c = { 0 };
	c.width = w;
	c.height = h;
	c.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
	uint32_t handles[4] = { c.handle }, pitches[4] = { c.pitch }, offsets[4] = { 0 };
	if (drmModeAddFB2(fd, w, h, fourcc, handles, pitches, offsets, &b->fb, 0))
	{
		struct drm_mode_destroy_dumb d = { 0 };
		d.handle = c.handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		return false;
	}
	b->handle = c.handle;
	b->pitch = c.pitch;
	b->size = c.size;
	return true;
}

static void buf_free(Buf *b)
{
	if (b->fb) drmModeRmFB(fd, b->fb);
	if (b->handle)
	{
		struct drm_mode_destroy_dumb d = { 0 };
		d.handle = b->handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(b, 0, sizeof *b);
}

// ---- the target display -------------------------------------------------------------------------------------------

static uint32_t target_conn, target_crtc, target_crtc_index;
static drmModeModeInfo target_mode;
static bool have_mode;
static uint32_t mode_blob;
static Plane *primary, *overlay;
static bool overlay_ar24;
static uint32_t panel_fmt = DRM_FORMAT_XRGB8888;

static bool pick_target(drmModeRes *res)
{
	// the target: the connector named by --connector when given, else the connected DSI panel, else any
	// connected connector with a mode. A score, not a pass order, so a named connector is never overridden.
	uint32_t want_conn = 0, want_enc = 0;
	drmModeConnector *want = NULL;
	int best = -1;
	for (int i = 0; i < res->count_connectors; i++)
	{
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		char name[32];
		conn_name(c->connector_type, c->connector_type_id, name);
		if (opt_connector && strcmp(name, opt_connector)) { drmModeFreeConnector(c); continue; }
		int score = 0;
		if (c->connection == DRM_MODE_CONNECTED) score += c->count_modes > 0 ? 4 : 2;
		if (c->connector_type == DRM_MODE_CONNECTOR_DSI) score += 2;
		if (score > best)
		{
			if (want) drmModeFreeConnector(want);
			best = score;
			want = c;
			want_conn = c->connector_id;
			want_enc = c->encoder_id;
		}
		else drmModeFreeConnector(c);
	}
	if (!want) return false;
	target_conn = want_conn;
	if (want->count_modes > 0)
	{
		target_mode = want->modes[0];
		for (int m = 0; m < want->count_modes; m++)
			if (want->modes[m].type & DRM_MODE_TYPE_PREFERRED) { target_mode = want->modes[m]; break; }
		for (int m = 0; m < want->count_modes; m++)
			if (want->modes[m].type & DRM_MODE_TYPE_USERDEF) { target_mode = want->modes[m]; break; }
		have_mode = true;
	}
	// the CRTC: the connector's current encoder first, else any encoder's possible CRTC
	uint32_t crtc = 0;
	if (want_enc)
	{
		drmModeEncoder *e = drmModeGetEncoder(fd, want_enc);
		if (e) { crtc = e->crtc_id; drmModeFreeEncoder(e); }
	}
	for (int i = 0; i < want->count_encoders && !crtc; i++)
	{
		drmModeEncoder *e = drmModeGetEncoder(fd, want->encoders[i]);
		if (!e) continue;
		for (int k = 0; k < res->count_crtcs && !crtc; k++)
			if (e->possible_crtcs & (1u << k)) crtc = res->crtcs[k];
		drmModeFreeEncoder(e);
	}
	if (!crtc && res->count_crtcs) crtc = res->crtcs[0];
	target_crtc = crtc;
	target_crtc_index = 0;
	for (int k = 0; k < res->count_crtcs; k++)
		if (res->crtcs[k] == crtc) target_crtc_index = (uint32_t)k;
	if (!have_mode && crtc)
	{
		drmModeCrtc *c = drmModeGetCrtc(fd, crtc);
		if (c && c->mode_valid) { target_mode = c->mode; have_mode = true; }
		if (c) drmModeFreeCrtc(c);
	}
	drmModeFreeConnector(want);
	if (!target_crtc) return false;
	if (have_mode && drmModeCreatePropertyBlob(fd, &target_mode, sizeof target_mode, &mode_blob))
		mode_blob = 0;
	return true;
}

// ---- reporting ----------------------------------------------------------------------------------------------------

static void print_connectors(drmModeRes *res)
{
	printf("\nconnectors\n");
	for (int i = 0; i < res->count_connectors; i++)
	{
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c) continue;
		char name[32];
		conn_name(c->connector_type, c->connector_type_id, name);
		printf("  %s (id %u) %s, encoder %u, %d mode(s)%s\n", name, c->connector_id,
		       c->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected", c->encoder_id, c->count_modes,
		       c->connector_id == target_conn ? "   <- target" : "");
		for (int m = 0; m < c->count_modes && m < 8; m++)
			printf("    mode %d: %ux%u @ %u Hz \"%s\"%s%s\n", m, c->modes[m].hdisplay, c->modes[m].vdisplay,
			       c->modes[m].vrefresh, c->modes[m].name,
			       c->modes[m].type & DRM_MODE_TYPE_PREFERRED ? " preferred" : "",
			       c->modes[m].type & DRM_MODE_TYPE_USERDEF ? " userdef" : "");
		uint64_t v = 0;
		drmModePropertyPtr p = get_prop(c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "panel orientation", &v);
		if (p)
		{
			printf("    panel orientation: %" PRIu64 " (%s)%s\n", v, orientation_name(v),
			       (p->flags & DRM_MODE_PROP_IMMUTABLE) ? " immutable" : "");
			drmModeFreeProperty(p);
		}
		uint64_t crtc_id = 0;	// the connector's CRTC (CRTC_ID property, atomic)
		p = get_prop(c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &crtc_id);
		if (p)
		{
			printf("    CRTC_ID: %" PRIu64 "\n", crtc_id);
			drmModeFreeProperty(p);
		}
		drmModeFreeConnector(c);
	}
}

static void print_crtcs(drmModeRes *res)
{
	printf("\ncrtcs\n");
	for (int i = 0; i < res->count_crtcs; i++)
	{
		drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
		if (!c) continue;
		printf("  CRTC %u (index %d)%s%s\n", c->crtc_id, i, c->mode_valid ? " active" : "",
		       c->crtc_id == target_crtc ? "   <- target" : "");
		if (c->mode_valid)
			printf("    mode: %ux%u @ %u Hz \"%s\"\n", c->mode.hdisplay, c->mode.vdisplay, c->mode.vrefresh,
			       c->mode.name);
		uint64_t v = 0;
		drmModePropertyPtr p = get_prop(c->crtc_id, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT_SIZE", &v);
		if (p)
		{
			printf("    GAMMA_LUT_SIZE: %" PRIu64 "\n", v);
			drmModeFreeProperty(p);
		}
		bool out_fence = false;
		p = get_prop(c->crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", &v);
		if (p)
		{
			out_fence = true;
			drmModeFreeProperty(p);
		}
		printf("    OUT_FENCE_PTR: %s\n", out_fence ? "yes" : "no");
		drmModeFreeCrtc(c);
	}
}

static void print_planes(drmModeRes *res)
{
	printf("\nplanes (advertised; the atomic tests below decide what is actually accepted)\n");
	for (int i = 0; i < nplanes; i++)
	{
		Plane *p = &planes[i];
		printf("  plane %u %s  possible CRTCs:", p->id, plane_type_name(p->type));
		bool any = false;
		for (int k = 0; k < res->count_crtcs; k++)
			if (supports_crtc(p, (uint32_t)k))
			{
				printf(" %u", res->crtcs[k]);
				any = true;
			}
		printf("%s\n", any ? "" : " none");
		if (p->type == DRM_PLANE_TYPE_PRIMARY && p == primary) printf("    (the target's primary)\n");
		if (p->type == DRM_PLANE_TYPE_OVERLAY && p == overlay) printf("    (the target's first overlay)\n");
		printf("    formats:");
		for (int f = 0; f < p->nformats; f++)
		{
			char n[16];
			printf(" %s", fourcc_name(p->formats[f], n));
		}
		printf("%s\n", p->has_in_formats ? "" : "   (no IN_FORMATS blob)");
		printf("    linear: AR24 %s, XR24 %s\n", p->linear_ar24 ? "yes" : "no", p->linear_xr24 ? "yes" : "no");
		if (p->zpos_id)
			printf("    zpos: %" PRIu64 "..%" PRIu64 "%s\n", p->zpos_min, p->zpos_max,
			       p->zpos_immutable ? " (immutable)" : "");
		else
			printf("    zpos: none\n");
		if (p->has_alpha)
			printf("    alpha: %" PRIu64 "..%" PRIu64 "\n", p->alpha_min, p->alpha_max);
		printf("    blend:");
		if (!p->nblends) printf(" none");
		for (int b = 0; b < p->nblends; b++)
		{
			uint64_t v = p->blends[b];
			printf(" %s(%" PRIu64 ")", v == BLEND_PREMULTI ? "premultiplied"
			       : v == BLEND_COVERAGE ? "coverage" : v == BLEND_PIXEL_NONE ? "none" : "?", v);
		}
		printf("\n    rotation:");
		if (!p->nrotations) printf(" none");
		for (int r = 0; r < p->nrotations; r++)
		{
			char n[96];
			printf(" %s", rotation_name(p->rotations[r], n));
		}
		printf("\n    fences: IN_FENCE_FD %s, OUT_FENCE_PTR %s\n", p->has_in_fence ? "yes" : "no",
		       p->has_out_fence ? "yes" : "no");
	}
}

// ---- the atomic matrix --------------------------------------------------------------------------------------------

static void addp(drmModeAtomicReq *r, uint32_t obj, uint32_t prop, uint64_t v)
{
	if (r && prop) drmModeAtomicAddProperty(r, obj, prop, v);
}

static void base_req(drmModeAtomicReq *r)
{
	addp(r, target_conn, prop_id(target_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), target_crtc);
	if (mode_blob)
	{
		addp(r, target_crtc, prop_id(target_crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), mode_blob);
		addp(r, target_crtc, prop_id(target_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
	}
}

static void plane_req(drmModeAtomicReq *r, const Plane *p, uint32_t fb, uint32_t sw, uint32_t sh,
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

// the only commit in this program: TEST_ONLY. No page flip, no event, no hardware change.
static int commit_test(drmModeAtomicReq *r)
{
	if (!r) return ENOMEM;
	errno = 0;
	int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	int err = errno;
	drmModeAtomicFree(r);
	return ret ? (err ? err : EINVAL) : 0;
}

static void result(const char *name, int err, bool *ok)
{
	if (!err) { printf("  [ok]     %s\n", name); if (ok) *ok = true; return; }
	printf("  [reject] %s: %s (%d)\n", name, strerror(err), err);
	if (err == EACCES) printf("           not DRM master (the atomic ioctl requires it, TEST_ONLY included)\n");
	if (err == EINVAL) printf("           the driver rejected a property, format, modifier or the combination\n");
	if (err == ENOSPC) printf("           no plane or bandwidth left for that combination\n");
	if (err == EBUSY) printf("           the display is in use by another commit or master\n");
}

static bool tests_ran;
static int tests_run;
static bool overlay_combo_ok, primary_rotation_ok, overlay_rotation_ok, scaling_ok;

static void run_atomic_tests(void)
{
	printf("\natomic tests (TEST_ONLY)\n");
	Buf panel = { 0 }, land = { 0 }, over = { 0 }, small = { 0 };
	uint32_t pw = target_mode.hdisplay, ph = target_mode.vdisplay;
	bool have_panel = buf_new(pw, ph, panel_fmt, &panel);
	bool have_land = buf_new(ph, pw, panel_fmt, &land);	// the game's landscape buffer
	uint32_t ofmt = overlay && overlay_ar24 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	bool have_over = overlay && buf_new(600, 200, ofmt, &over);
	bool have_small = buf_new(640, 480, panel_fmt, &small);
	if (!have_panel) printf("  [skip]   cannot make a %ux%u dumb framebuffer\n", pw, ph);

	drmModeAtomicReq *r;
	if (have_panel)
	{
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, panel.fb, pw, ph, pw, ph, DRM_MODE_ROTATE_0);
		result("modeset: primary at the panel's mode", commit_test(r), NULL);
		tests_run++;
	}
	if (have_panel && have_over)
	{
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, panel.fb, pw, ph, pw, ph, DRM_MODE_ROTATE_0);
		plane_req(r, overlay, over.fb, 600, 200, 600, 200, DRM_MODE_ROTATE_0);
		if (!overlay->zpos_immutable && overlay->zpos_id) addp(r, overlay->id, overlay->zpos_id, overlay->zpos_max);
		if (overlay->alpha_id) addp(r, overlay->id, overlay->alpha_id, 0x8000);
		if (overlay->blend_id && blend_supports(overlay, BLEND_PREMULTI))
			addp(r, overlay->id, overlay->blend_id, BLEND_PREMULTI);
		result("primary + overlay (zpos top, alpha 50%, premultiplied)", commit_test(r), &overlay_combo_ok);
		tests_run++;
		if (ofmt == DRM_FORMAT_XRGB8888)
			printf("           note: the overlay has no AR24; alpha was not tested (opaque card only)\n");
	}
	if (have_panel && have_over && rotation_supports(overlay, DRM_MODE_ROTATE_90))
	{
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, panel.fb, pw, ph, pw, ph, DRM_MODE_ROTATE_0);
		plane_req(r, overlay, over.fb, 600, 200, 200, 600, DRM_MODE_ROTATE_90);
		result("overlay rotated 90 (informational)", commit_test(r), &overlay_rotation_ok);
		tests_run++;
	}
	else if (!have_over) printf("  [skip]   overlay rotate-90: no overlay buffer\n");
	else printf("  [skip]   overlay rotate-90 not advertised\n");
	if (have_panel && have_land && rotation_supports(primary, DRM_MODE_ROTATE_90))
	{
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, land.fb, ph, pw, pw, ph, DRM_MODE_ROTATE_90);	// landscape -> portrait, 1:1
		result("primary rotated 90 (the game buffer: the zero-copy gate)", commit_test(r), &primary_rotation_ok);
		tests_run++;
	}
	else if (!have_land) printf("  [skip]   primary rotate-90: no landscape buffer\n");
	else printf("  [skip]   primary rotate-90 not advertised\n");
	if (have_panel && have_small)
	{
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, small.fb, 640, 480, pw, ph, DRM_MODE_ROTATE_0);
		result("primary scaled 640x480 -> the panel", commit_test(r), &scaling_ok);
		tests_run++;
	}
	if (have_panel && have_over)
	{
		// the toast band is scaled down: check the scaler accepts it on the overlay plane
		r = drmModeAtomicAlloc();
		base_req(r);
		plane_req(r, primary, panel.fb, pw, ph, pw, ph, DRM_MODE_ROTATE_0);
		plane_req(r, overlay, over.fb, 600, 200, 300, 400, DRM_MODE_ROTATE_0);
		result("overlay scaled 600x200 -> 300x400 (informational)", commit_test(r), NULL);
		tests_run++;
	}

	buf_free(&panel);
	buf_free(&land);
	buf_free(&over);
	buf_free(&small);
	tests_ran = tests_run > 0;
}

// ---- main ---------------------------------------------------------------------------------------------------------

static void usage(void)
{
	fprintf(stderr,
	        "usage: drm-plane-probe [--card /dev/dri/cardN] [--connector DSI-1] [--take-master] [--no-atomic]\n"
	        "  (enumeration is read-only; the atomic matrix needs DRM master and uses TEST_ONLY commits only)\n");
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--card") && i + 1 < argc) opt_card = argv[++i];
		else if (!strcmp(argv[i], "--connector") && i + 1 < argc) opt_connector = argv[++i];
		else if (!strcmp(argv[i], "--take-master")) opt_take_master = true;
		else if (!strcmp(argv[i], "--no-atomic")) opt_no_atomic = true;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
		else { usage(); return 1; }
	}

	fd = open(opt_card, O_RDWR | O_CLOEXEC);
	if (fd < 0)
	{
		fprintf(stderr, "drm-plane-probe: cannot open %s: %s\n", opt_card, strerror(errno));
		return 1;
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
	{
		fprintf(stderr, "drm-plane-probe: this card has no universal planes or atomic KMS: %s\n", strerror(errno));
		return 1;
	}

	drmVersionPtr ver = drmGetVersion(fd);
	struct utsname un;
	uname(&un);
	printf("banana drm-plane-probe\n");
	printf("kernel : %s %s %s\n", un.sysname, un.release, un.machine);
	if (ver) printf("driver : %s %d.%d.%d (%s) \"%s\"\n", ver->name, ver->version_major, ver->version_minor,
	                ver->version_patchlevel, ver->date, ver->desc ? ver->desc : "");
	printf("card   : %s\n", opt_card);
	drmFreeVersion(ver);

	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
	{
		fprintf(stderr, "drm-plane-probe: drmModeGetResources failed: %s\n", strerror(errno));
		return 1;
	}
	printf("objects: %d connector(s), %d encoder(s), %d CRTC(s), %d framebuffer(s)\n",
	       res->count_connectors, res->count_encoders, res->count_crtcs, res->count_fbs);

	if (!pick_target(res))
	{
		fprintf(stderr, "drm-plane-probe: no usable display connector (try --connector NAME)\n");
		return 1;
	}

	drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
	if (!pres)
	{
		fprintf(stderr, "drm-plane-probe: drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return 1;
	}
	for (uint32_t i = 0; i < pres->count_planes; i++)
	{
		drmModePlane *pl = drmModeGetPlane(fd, pres->planes[i]);
		if (pl) { read_plane(pl); drmModeFreePlane(pl); }
	}
	drmModeFreePlaneResources(pres);

	// the overlay: preferably one that takes linear AR24, and of those the topmost (highest zpos), since a plane's
	// stacking order is fixed on some drivers
	Plane *overlay_plain = NULL, *overlay_ar = NULL;
	for (int i = 0; i < nplanes; i++)
	{
		Plane *p = &planes[i];
		if (!supports_crtc(p, target_crtc_index)) continue;
		if (p->type == DRM_PLANE_TYPE_PRIMARY && !primary) primary = p;
		if (p->type == DRM_PLANE_TYPE_OVERLAY)
		{
			if (!overlay_plain) overlay_plain = p;
			if (format_listed(p, DRM_FORMAT_ARGB8888) && p->linear_ar24 &&
			    (!overlay_ar || !overlay_ar->has_zpos || (p->has_zpos && p->zpos_value >= overlay_ar->zpos_value)))
				overlay_ar = p;
		}
	}
	overlay = overlay_ar ? overlay_ar : overlay_plain;
	overlay_ar24 = overlay && format_listed(overlay, DRM_FORMAT_ARGB8888) && overlay->linear_ar24;

	// a dumb buffer is linear 32 bpp: the primary's format, if the driver does not list XR24
	panel_fmt = (primary && format_listed(primary, DRM_FORMAT_XRGB8888) && primary->linear_xr24)
		? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888;

	print_connectors(res);
	print_crtcs(res);
	print_planes(res);

	printf("\ntarget\n");
	printf("  connector %u, CRTC %u (index %u)%s\n", target_conn, target_crtc, target_crtc_index,
	       have_mode ? "" : " (no mode; the display is off or disconnected)");
	printf("  primary plane: %s\n", primary ? "found" : "NONE");
	printf("  overlay plane: %s%s\n", overlay ? "found" : "NONE",
	       overlay ? (overlay_ar24 ? " (AR24 advertised)" : " (no AR24: opaque only)") : "");
	if (primary && overlay && primary->has_zpos && overlay->has_zpos)
	{
		printf("  zpos: primary %" PRIu64 ", overlay %" PRIu64 "%s\n", primary->zpos_value, overlay->zpos_value,
		       overlay->zpos_value > primary->zpos_value ? "" : "  WARNING: the overlay is not above the primary");
	}
	if (primary)
	{
		char n[96];
		printf("  primary rotation advertised: %s\n", rotation_supports(primary, DRM_MODE_ROTATE_90)
		       ? rotation_name(DRM_MODE_ROTATE_90, n) : "rotate-90 is NOT advertised");
	}
	if (!have_mode) printf("  the atomic matrix cannot run without a mode\n");
	if (!mode_blob) printf("  no mode blob: the matrix would run against the current mode (or be skipped)\n");

	bool master = drmIsMaster(fd) == 1, took = false;
	if (!opt_no_atomic && have_mode && primary)
	{
		if (!master && opt_take_master)
		{
			if (drmSetMaster(fd)) printf("\n  cannot take DRM master: %s (is banana-ui running?)\n", strerror(errno));
			else
			{
				master = true;
				took = true;
				printf("\n  took DRM master (TEST_ONLY commits only; the display is not changed).\n");
			}
		}
		if (!master)
			printf("\natomic tests: SKIPPED -- not DRM master.\n"
			       "  The kernel requires master for DRM_IOCTL_MODE_ATOMIC even with TEST_ONLY. Stop the menu in a\n"
			       "  way init cannot respawn (the bind-mount procedure in docs/PLAN_DRM_COMPOSITOR.md, 1.4), then\n"
			       "  rerun with --take-master.\n");
		else run_atomic_tests();
	}
	if (took) drmDropMaster(fd);
	if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);

	if (tests_ran && overlay_combo_ok && primary_rotation_ok)
		printf("\nVERDICT: GO (zero-copy) -- the overlay combination and primary rotation were both accepted.\n");
	else if (tests_ran && overlay_combo_ok)
		printf("\nVERDICT: GO (EGL rotate) -- the overlay works, primary rotation was not validated; budget one EGL pass.\n");
	else if (tests_ran)
		printf("\nVERDICT: NO-GO -- the overlay combination was rejected; retain cage.\n");
	else
		printf("\nVERDICT: UNDETERMINED -- the atomic matrix did not run; do not treat this as a capability result.\n");

	drmModeFreeResources(res);
	close(fd);
	return tests_ran ? 0 : 2;
}
