#include "kms.h"
#include "turn.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace banana
{
	static_assert(sizeof(drmModeModeInfo) == 68, "mode size");

	static void flipDone(int, unsigned, unsigned, unsigned, void* data)
	{
		int* pending = (int*)data;
		if (*pending > 0) (*pending)--;
	}

	// the property by name on an object: the current value in *value, the property itself returned (free with
	// drmModeFreeProperty), or nullptr
	static drmModePropertyPtr getProp(int fd, uint32_t obj, uint32_t type, const char* name, uint64_t* value)
	{
		drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj, type);
		if (!props) return nullptr;
		drmModePropertyPtr found = nullptr;
		for (uint32_t i = 0; i < props->count_props && !found; i++)
		{
			drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
			if (!p) continue;
			if (!strcmp(p->name, name)) { if (value) *value = props->prop_values[i]; found = p; }
			else drmModeFreeProperty(p);
		}
		drmModeFreeObjectProperties(props);
		return found;
	}

	uint32_t Display::propId(uint32_t obj, uint32_t type, const char* name, uint64_t* value)
	{
		drmModePropertyPtr p = getProp(fd, obj, type, name, value);
		if (!p) return 0;
		uint32_t id = p->prop_id;
		drmModeFreeProperty(p);
		return id;
	}

	bool Display::makeBuf(Buf& b, uint32_t fourcc)
	{
		drm_mode_create_dumb c = {};
		c.width = SW; c.height = SH; c.bpp = 32;
		if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
		// the buffer is assigned only on success, so the caller's Buf is never half-initialised
		auto destroy = [&](uint32_t fb) {
			if (fb) drmModeRmFB(fd, fb);
			drm_mode_destroy_dumb d = {};
			d.handle = c.handle;
			drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		};
		uint32_t handles[4] = { c.handle }, pitches[4] = { c.pitch }, offsets[4] = { 0 };
		uint32_t fb = 0;
		if (drmModeAddFB2(fd, SW, SH, fourcc, handles, pitches, offsets, &fb, 0))
		{
			destroy(0);
			return false;
		}
		drm_mode_map_dumb m = {};
		m.handle = c.handle;
		void* p = nullptr;
		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) == 0)
			p = mmap(nullptr, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
		if (p == MAP_FAILED || !p)
		{
			destroy(fb);
			return false;
		}
		b.handle = c.handle; b.pitch = c.pitch; b.size = c.size; b.fb = fb;
		b.map = (uint8_t*)p;
		memset(b.map, 0, b.size);
		return true;
	}

	void Display::freeBuf(Buf& b)
	{
		if (b.map) munmap(b.map, b.size);
		if (b.fb) drmModeRmFB(fd, b.fb);
		if (b.handle) { drm_mode_destroy_dumb d = {}; d.handle = b.handle; drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); }
		b = Buf();
	}

	// how the panel is mounted, from the connector's "panel orientation" (the device tree's rotation): the degrees
	// present() turns the menu by. banana.rotate=0|90|180|270 on the kernel command line overrides it, for a panel
	// that reports the wrong one.
	static int panelRotation(int fd, uint32_t conn)
	{
		int rot = 0;
		if (drmModeObjectProperties* props = drmModeObjectGetProperties(fd, conn, DRM_MODE_OBJECT_CONNECTOR))
		{
			for (uint32_t i = 0; i < props->count_props; i++)
			{
				drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
				if (!p) continue;
				if (!strcmp(p->name, "panel orientation"))
				{
					// 0 normal, 1 upside down, 2 left side up, 3 right side up (the device tree's rotation is counter
					// clockwise: rotation=<90> is RIGHT_UP, and the image then turns 90 clockwise)
					uint64_t v = props->prop_values[i];
					rot = v == 1 ? 180 : v == 2 ? 270 : v == 3 ? 90 : 0;
				}
				drmModeFreeProperty(p);
			}
			drmModeFreeObjectProperties(props);
		}
		if (FILE* f = fopen("/proc/cmdline", "r"))
		{
			char line[1024] = "";
			if (fgets(line, sizeof line, f))
				for (char* p = strstr(line, "banana.rotate="); p; p = strstr(p + 1, "banana.rotate="))
				{
					int v = atoi(p + strlen("banana.rotate="));
					if (v == 0 || v == 90 || v == 180 || v == 270) rot = v;
				}
			fclose(f);
		}
		return rot;
	}

	// the part of the composed screen that changed, into a scanout buffer the panel turns by rot: a turned rectangle
	// is written a panel row at a time (contiguous writes, strided reads of the cached screen). The quarter turns are
	// a transpose, done four pixels at a time -- see src/turn.h (and its unit test, tests/turn-test.cpp).
	//
	// Coordinates (Phase 3A):
	//   composed menu canvas: W x H = 1240 x 1080 (theme layout, touch already mapped to this)
	//   panel mode / scanout buffers: SW x SH = 1080 x 1240 (portrait native)
	//   rot 90 (the RPC): canvas (x, y) -> panel (x, H-1-y); the top row of the canvas becomes the right column.
	//   A future game client draws 1240 x 1080 too; its rotation is the same 90 degrees in the EGL presenter.
	void Display::turn(const uint32_t* src, Buf& bf, const Damage& d)
	{
		turnRect(src, bf.map, bf.pitch, W, H, rot, d.x0, d.y0, d.x1, d.y1);
	}

	// The planes this display will use: found by type and possible_crtcs, never by a hard-coded id. The primary is
	// the first PRIMARY that can feed the CRTC; the overlay the first OVERLAY. Their property ids are read here so
	// the atomic modeset and page flips do not repeat the lookups.
	bool Display::findPlanes(Panel& pn, uint32_t crtc_index)
	{
		drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
		if (!pr) return false;
		uint32_t bit = 1u << crtc_index;
		for (uint32_t i = 0; i < pr->count_planes; i++)
		{
			drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
			if (!pl) continue;
			if (!(pl->possible_crtcs & bit)) { drmModeFreePlane(pl); continue; }
			uint64_t type = 0;
			if (!propId(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type)) { drmModeFreePlane(pl); continue; }
			Plane* dst = type == DRM_PLANE_TYPE_PRIMARY && !pn.primary.id ? &pn.primary : (type == DRM_PLANE_TYPE_OVERLAY && !pn.overlay.id ? &pn.overlay : nullptr);
			if (!dst) { drmModeFreePlane(pl); continue; }
			dst->id = pl->plane_id;
			dst->type = (uint32_t)type;
			dst->crtc_id = propId(dst->id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
			dst->fb_id = propId(dst->id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			dst->src_x = propId(dst->id, DRM_MODE_OBJECT_PLANE, "SRC_X");
			dst->src_y = propId(dst->id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
			dst->src_w = propId(dst->id, DRM_MODE_OBJECT_PLANE, "SRC_W");
			dst->src_h = propId(dst->id, DRM_MODE_OBJECT_PLANE, "SRC_H");
			dst->crt_x = propId(dst->id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
			dst->crt_y = propId(dst->id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
			dst->crt_w = propId(dst->id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
			dst->crt_h = propId(dst->id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
			dst->in_fence_fd = propId(dst->id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");
			uint64_t v = 0;
			if (drmModePropertyPtr zp = getProp(fd, dst->id, DRM_MODE_OBJECT_PLANE, "zpos", &v))
			{
				dst->zpos = zp->prop_id;
				dst->zpos_mutable = !(zp->flags & DRM_MODE_PROP_IMMUTABLE);
				if (zp->count_values >= 2) dst->zpos_max = zp->values[1];
				drmModeFreeProperty(zp);
			}
			if (drmModePropertyPtr ap = getProp(fd, dst->id, DRM_MODE_OBJECT_PLANE, "alpha", &v))
			{
				dst->alpha = ap->prop_id;
				dst->has_alpha = true;
				drmModeFreeProperty(ap);
			}
			if (drmModePropertyPtr bp = getProp(fd, dst->id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", &v))
			{
				dst->blend = bp->prop_id;
				dst->has_blend = true;
				drmModeFreeProperty(bp);
			}
			drmModeFreePlane(pl);
		}
		drmModeFreePlaneResources(pr);
		return pn.primary.id && pn.overlay.id;
	}

	// The first commit: connector CRTC + mode, primary black, overlay menu. TEST_ONLY first; ALLOW_MODESET only when
	// the driver accepts it. A failure here makes openSingle fall back to the legacy single-primary path.
	bool Display::atomicModeset(Panel& pn)
	{
		if (!pn.atomic || !pn.primary.id || !pn.overlay.id || !pn.mode_blob || !pn.black.fb || !pn.buf[pn.front].fb) return false;
		drmModeAtomicReq* r = drmModeAtomicAlloc();
		if (!r) return false;
		auto addp = [&](uint32_t obj, uint32_t prop, uint64_t v) { if (prop) drmModeAtomicAddProperty(r, obj, prop, v); };
		auto addPlane = [&](const Plane& pl, uint32_t fb, bool over) {
			addp(pl.id, pl.crtc_id, pn.crtc);
			addp(pl.id, pl.fb_id, fb);
			addp(pl.id, pl.src_x, 0);
			addp(pl.id, pl.src_y, 0);
			addp(pl.id, pl.src_w, (uint64_t)SW << 16);
			addp(pl.id, pl.src_h, (uint64_t)SH << 16);
			addp(pl.id, pl.crt_x, 0);
			addp(pl.id, pl.crt_y, 0);
			addp(pl.id, pl.crt_w, SW);
			addp(pl.id, pl.crt_h, SH);
			if (pl.zpos_mutable) addp(pl.id, pl.zpos, over ? pl.zpos_max : 0);
			if (over)
			{
				if (pl.has_alpha) addp(pl.id, pl.alpha, fb == maskFb() ? maskPlaneAlpha(maskPct) : 0xffff);	// the menu pixels are opaque
				if (pl.has_blend) addp(pl.id, pl.blend, 0);	// premultiplied
			}
		};
		addp(pn.conn, propId(pn.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), pn.crtc);
		addp(pn.crtc, propId(pn.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), pn.mode_blob);
		addp(pn.crtc, propId(pn.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
		// the LUT is always attached: it keeps the DSPP reserved, so the CTM can come and go without a modeset
		if (pn.lut_size && colorRebuild(pn))
		{
			addp(pn.crtc, pn.crtc_ctm, pn.color.ctm);
			addp(pn.crtc, pn.crtc_gamma_lut, pn.color.lut);
		}
		// with a mask up the menu moves to the primary, since there is only the one overlay
		addPlane(pn.primary, maskFb() ? pn.buf[pn.front].fb : pn.black.fb, false);
		addPlane(pn.overlay, maskFb() ? maskFb() : pn.buf[pn.front].fb, true);
		int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
		if (ret)
		{
			fprintf(stderr, "kms: atomic modeset rejected (%s); legacy menu path\n", strerror(errno));
			drmModeAtomicFree(r);
			return false;
		}
		ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
		drmModeAtomicFree(r);
		if (ret) fprintf(stderr, "kms: atomic modeset failed (%s); legacy menu path\n", strerror(errno));
		return ret == 0;
	}

	// one panel: the connector named like top when it is connected, else any connected connector with a mode (the
	// type is only a preference here)
	bool Display::openSingle(const std::string& top, std::string& err)
	{
		close();
		std::vector<std::string> cards;
		if (DIR* d = opendir("/dev/dri"))
		{
			while (dirent* e = readdir(d))
				if (!strncmp(e->d_name, "card", 4)) cards.push_back(std::string("/dev/dri/") + e->d_name);
			closedir(d);
		}
		std::sort(cards.begin(), cards.end());
		// the panel's connector type from the name ("DSI-1"; "Virtual-1" on QEMU's virtio GPU)
		std::string type = top.substr(0, top.find('-'));
		err = "no DRM device with a connected " + type + " panel";
		for (auto& path : cards)
		{
			int f = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
			if (f < 0) continue;
			drmModeRes* res = drmModeGetResources(f);
			if (!res) { ::close(f); continue; }
			struct Found { uint32_t conn, crtc, crtc_index; drmModeModeInfo mode; bool top; };
			std::vector<Found> found;
			// the connector to take: the named type first, else any connected one
			for (int pass = 0; pass < 2 && found.empty(); pass++)
			{
				for (int i = 0; i < res->count_connectors; i++)
				{
					drmModeConnector* c = drmModeGetConnector(f, res->connectors[i]);
					if (!c) continue;
					const char* tn = drmModeGetConnectorTypeName(c->connector_type);
					bool take = tn && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
					if (take && type != tn) take = pass == 1;
					if (take)
					{
						// the mode: the kernel command line's (video=<connector>:<mode>) first, then the panel's preferred one
						drmModeModeInfo mode = c->modes[0];
						for (int m = 0; m < c->count_modes; m++)
							if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = c->modes[m]; break; }
						for (int m = 0; m < c->count_modes; m++)
							if (c->modes[m].type & DRM_MODE_TYPE_USERDEF) { mode = c->modes[m]; break; }
						// a CRTC one of this connector's encoders can drive
						uint32_t crtc = 0, ci = 0;
						for (int e = 0; e < c->count_encoders && !crtc; e++)
						{
							drmModeEncoder* enc = drmModeGetEncoder(f, c->encoders[e]);
							if (!enc) continue;
							for (int k = 0; k < res->count_crtcs && !crtc; k++)
								if (enc->possible_crtcs & (1u << k)) { crtc = res->crtcs[k]; ci = (uint32_t)k; }
							drmModeFreeEncoder(enc);
						}
						if (crtc)
						{
							char name[32];
							snprintf(name, sizeof name, "%s-%u", tn, c->connector_type_id);
							found.push_back({ c->connector_id, crtc, ci, mode, top == name });
						}
					}
					drmModeFreeConnector(c);
					if (!found.empty()) break;
				}
			}
			drmModeFreeResources(res);
			if (found.empty()) { ::close(f); continue; }
			fd = f;
			rot = panelRotation(f, found[0].conn);
			SW = found[0].mode.hdisplay; SH = found[0].mode.vdisplay;
			// a panel mounted turned: the menu draws the other way round (1240x1080 on a 1080x1240 panel)
			W = rot == 90 || rot == 270 ? SH : SW;
			H = rot == 90 || rot == 270 ? SW : SH;
			panel = Panel();
			panel.conn = found[0].conn; panel.crtc = found[0].crtc;
			memcpy(panel.mode, &found[0].mode, sizeof panel.mode);

			// The menu belongs on the overlay plane when the driver exposes one and accepts atomic KMS: the primary
			// stays black, the overlay carries the ARGB menu (alpha 255, premultiplied, zpos top). Discovered by type
			// and possible_crtcs at runtime; a driver without the pair keeps the old single-plane path.
			bool atomic = drmSetClientCap(f, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
			              drmSetClientCap(f, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
			if (atomic) atomic = findPlanes(panel, found[0].crtc_index);
			panel.atomic = atomic;
			if (atomic)
			{
				uint64_t size = 0;
				panel.crtc_ctm = propId(panel.crtc, DRM_MODE_OBJECT_CRTC, "CTM");
				panel.crtc_gamma_lut = propId(panel.crtc, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT");
				if (panel.crtc_gamma_lut && propId(panel.crtc, DRM_MODE_OBJECT_CRTC, "GAMMA_LUT_SIZE", &size) &&
				    size >= 2 && size <= kMaxLut)
					panel.lut_size = (uint32_t)size;
			}
			if (atomic)
			{
				if (drmModeCreatePropertyBlob(f, &panel.mode, sizeof panel.mode, &panel.mode_blob) ||
				    !makeBuf(panel.buf[0], DRM_FORMAT_ARGB8888) || !makeBuf(panel.buf[1], DRM_FORMAT_ARGB8888) ||
				    !makeBuf(panel.black, DRM_FORMAT_XRGB8888) || !makeBuf(panel.card[0], DRM_FORMAT_ARGB8888) ||
				    !makeBuf(panel.card[1], DRM_FORMAT_ARGB8888))
				{
					panel.atomic = false;
					if (panel.mode_blob) { drmModeDestroyPropertyBlob(f, panel.mode_blob); panel.mode_blob = 0; }
					freeBuf(panel.black);
					freeBuf(panel.buf[0]); freeBuf(panel.buf[1]); freeBuf(panel.card[0]); freeBuf(panel.card[1]);
				}
			}
			if (!panel.atomic)
			{
				// the old path: one XRGB menu on the primary (QEMU's virtio, a driver without overlay/atomic)
				if (!makeBuf(panel.buf[0], DRM_FORMAT_XRGB8888) || !makeBuf(panel.buf[1], DRM_FORMAT_XRGB8888))
				{
					err = "can't make scanout buffers";
					close();
					return false;
				}
			}
			if (!restore())
			{
				if (panel.atomic)
				{
					// atomic was advertised but rejected: remake the menu as XRGB and use the primary
					panel.atomic = false;
					if (panel.mode_blob) { drmModeDestroyPropertyBlob(f, panel.mode_blob); panel.mode_blob = 0; }
					freeBuf(panel.black);
					freeBuf(panel.buf[0]); freeBuf(panel.buf[1]);
					if (!makeBuf(panel.buf[0], DRM_FORMAT_XRGB8888) || !makeBuf(panel.buf[1], DRM_FORMAT_XRGB8888) || !restore())
					{
						err = "can't take the panel (another master?)";
						close();
						return false;
					}
				}
				else
				{
					err = "can't take the panel (another master?)";
					close();
					return false;
				}
			}
			fprintf(stderr, "kms: panel %dx%d (mode %ux%u, rotation %d), atomic=%d primary=%u overlay=%u\n",
			        W, H, SW, SH, rot, panel.atomic, panel.primary.id, panel.overlay.id);
			return true;
		}
		return false;
	}

	/* false when the panel did not really come back: another DRM master (a game's compositor still letting go) makes
	 * SetMaster and SetCrtc fail, and the caller's retry loop is what waits it out */
	bool Display::restore()
	{
		if (fd < 0) return false;
		bool ok = drmSetMaster(fd) == 0;
		if (panel.atomic) { if (!atomicModeset(panel)) ok = false; }
		else if (drmModeSetCrtc(fd, panel.crtc, panel.buf[panel.front].fb, 0, 0, &panel.conn, 1, (drmModeModeInfo*)panel.mode) != 0)
			ok = false;
		if (ok)
		{
			// the blocking modeset waited out any flip still in flight, and its event may be queued unread: if it were
			// read after this, it would count against the next commit and the one after would fail EBUSY
			pollfd pf = { fd, POLLIN, 0 };
			while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) handleEvents();
			pending = 0;
			applyGamma();
		}
		return ok;
	}

	void Display::setWarmth(int level)
	{
		warmth = level;
		applyGamma();
	}

	bool Display::shaderSetColor(ColorProfile c)
	{
		if (c >= COLOR_PROFILE_COUNT) return false;
		colorProfile = c;
		if (fd < 0 || !panel.atomic || !panel.lut_size) return false;
		return colorRebuild(panel);
	}

	// The panel's colour as fresh blobs for the next commit to attach. Always new ids: the kernel only reprograms the
	// DSPP when the blob id changes, and neither the modeset after a wake nor the flip after it may be skipped.
	bool Display::colorRebuild(Panel& pn)
	{
		if (fd < 0 || !pn.lut_size) return false;
		ColorState& c = pn.color;
		for (uint32_t& id : c.retired)
		{
			if (id) drmModeDestroyPropertyBlob(fd, id);
			id = 0;
		}
		c.retired[0] = c.ctm;
		c.retired[1] = c.lut;
		c.ctm = c.lut = 0;
		colorLutFill(lutScratch, pn.lut_size, colorProfile, warmth);
		bool ok = drmModeCreatePropertyBlob(fd, lutScratch, pn.lut_size * sizeof(drm_color_lut), &c.lut) == 0;
		if (ok && colorProfile != COLOR_PROFILE_BYPASS && pn.crtc_ctm)
		{
			drm_color_ctm m;
			memcpy(m.matrix, kColorCtm[colorProfile], sizeof m.matrix);
			ok = drmModeCreatePropertyBlob(fd, &m, sizeof m, &c.ctm) == 0;
		}
		if (!ok) fprintf(stderr, "kms: colour: can't make a blob (%s)\n", strerror(errno));
		c.dirty = true;
		return ok;
	}

	// the night light: a linear ramp with green and blue pulled down, on every CRTC. Level 0 is the plain ramp, so
	// turning it off restores the panel's own look; the LUT is after the planes, so a game's frame is tinted too.
	// An atomic panel takes it as a GAMMA_LUT blob on the next commit: the legacy call below would clear the CTM.
	void Display::applyGamma()
	{
		if (fd < 0) return;
		double kg, kb;
		warmthGains(warmth, kg, kb);
		if (panel.atomic && panel.lut_size) { colorRebuild(panel); return; }
		drmModeCrtc* c = drmModeGetCrtc(fd, panel.crtc);
		if (!c) return;
		unsigned n = c->gamma_size;
		if (n > 1)
		{
			std::vector<uint16_t> r(n), g(n), b(n);
			for (unsigned i = 0; i < n; i++)
			{
				uint32_t v = (uint32_t)((uint64_t)i * 65535u / (n - 1));
				r[i] = (uint16_t)v;
				g[i] = (uint16_t)(v * kg);
				b[i] = (uint16_t)(v * kb);
			}
			if (drmModeCrtcSetGamma(fd, panel.crtc, n, r.data(), g.data(), b.data()) != 0)
				fprintf(stderr, "kms: night light: set gamma failed (%s)\n", strerror(errno));
		}
		drmModeFreeCrtc(c);
	}

	void Display::close()
	{
		if (fd < 0) return;
		Panel& pn = panel;
		freeBuf(pn.buf[0]); freeBuf(pn.buf[1]); freeBuf(pn.black); freeBuf(pn.card[0]); freeBuf(pn.card[1]);
		freeBuf(pn.frame[0]); freeBuf(pn.frame[1]);
		freeBuf(pn.mask[0]); freeBuf(pn.mask[1]);
		pn.frameOn = false;
		if (pn.mode_blob) { drmModeDestroyPropertyBlob(fd, pn.mode_blob); pn.mode_blob = 0; }
		for (uint32_t id : { pn.color.ctm, pn.color.lut, pn.color.retired[0], pn.color.retired[1] })
			if (id) drmModeDestroyPropertyBlob(fd, id);
		pn.color = ColorState();
		::close(fd);
		fd = -1;
		pending = 0;
		rot = 0; SW = SH = 0;
		maskMode = SHADER_MASK_OFF;
		baseStale = false;
		std::vector<uint8_t>().swap(maskCopy);
		std::vector<uint8_t>().swap(frameRaw);
		std::vector<uint8_t>().swap(frameCopy);
	}

	bool Display::present(const uint32_t* screen, const Damage& dmg)
	{
		if (fd < 0 || pending) return false;
		bool any = false;
		Panel& pn = panel;
		// the hidden buffer missed what changed since it was shown: this frame's part and the last frame's
		int back = pn.front ^ 1;
		Damage d = dmg;
		d.add(pn.missed[back]);
		pn.missed[back] = Damage();
		d.x0 = std::max(d.x0, 0); d.y0 = std::max(d.y0, 0);
		d.x1 = std::min(d.x1, W); d.y1 = std::min(d.y1, H);
		if (d.empty())
		{
			// nothing to draw, but new colour blobs still need a commit to reach the panel
			if (pn.atomic && pn.color.dirty && commitMenu(pn, pn.buf[pn.front].fb)) any = true;
			return any;
		}
		Buf& bf = pn.buf[back];
		if (rot)
			turn(screen, bf, d);
		else if (d.x0 == 0 && d.x1 == W && bf.pitch == (uint32_t)W * 4)
		{
			// a full-width change on an unpadded buffer is one contiguous copy, not a row at a time
			memcpy(bf.map + (size_t)d.y0 * bf.pitch, screen + (size_t)d.y0 * W, (size_t)(d.y1 - d.y0) * W * 4);
		}
		else
		{
			// only the columns that changed: the scanout buffer is uncached, every byte written costs
			size_t n = (size_t)(d.x1 - d.x0) * 4;
			for (int y = d.y0; y < d.y1; y++)
				memcpy(bf.map + (size_t)y * bf.pitch + (size_t)d.x0 * 4, screen + (size_t)y * W + d.x0, n);
		}
		pn.missed[pn.front] = dmg;	// the buffer going hidden misses this frame
		if (pn.atomic)
		{
			if (commitMenu(pn, bf.fb))
			{
				pn.front = back;
				any = true;
			}
			else
			{
				fprintf(stderr, "kms: atomic flip failed (%s); showing it directly\n", strerror(errno));
				drmModeSetCrtc(fd, pn.crtc, bf.fb, 0, 0, &pn.conn, 1, (drmModeModeInfo*)pn.mode);
				pn.front = back;
			}
		}
		else if (drmModePageFlip(fd, pn.crtc, bf.fb, DRM_MODE_PAGE_FLIP_EVENT, &pending) == 0)
		{
			pending++;
			pn.front = back;
			any = true;
		}
		else
		{
			// no flip (lost master, a modeset in the way): put the buffer up directly
			drmModeSetCrtc(fd, pn.crtc, bf.fb, 0, 0, &pn.conn, 1, (drmModeModeInfo*)pn.mode);
			pn.front = back;
		}
		return any;
	}

	// One atomic frame on a panel: the primary (a game buffer or the black one) and the overlay (the menu, or 0 to
	// hide it). srcW/srcH are the source size in pixels; the destination is always the panel mode, so a 1240x1080
	// client buffer is scaled to the 1080x1240 panel here (Phase 3B; 3C rotates it in EGL instead). The 3A menu
	// path passes the panel size for both and is pixel-identical to before.
	bool Display::commitFrame(Panel& pn, uint32_t primaryFb, uint32_t srcW, uint32_t srcH, uint32_t overlayFb, int fenceFd)
	{
		if (fd < 0 || pending || !pn.atomic) return false;
		AtomicBuf& r = atomicBuf;
		r.clear();
		auto addp = [&](uint32_t obj, uint32_t prop, uint64_t v) { r.add(obj, prop, v); };
		auto addPlane = [&](const Plane& pl, uint32_t fb, uint32_t sw, uint32_t sh, bool on, int ff) {
			addp(pl.id, pl.crtc_id, on ? pn.crtc : 0);
			addp(pl.id, pl.fb_id, fb);
			if (!on) return;
			addp(pl.id, pl.src_x, 0);
			addp(pl.id, pl.src_y, 0);
			addp(pl.id, pl.src_w, (uint64_t)sw << 16);
			addp(pl.id, pl.src_h, (uint64_t)sh << 16);
			addp(pl.id, pl.crt_x, 0);
			addp(pl.id, pl.crt_y, 0);
			addp(pl.id, pl.crt_w, SW);
			addp(pl.id, pl.crt_h, SH);
			if (pl.id == pn.overlay.id && pl.has_alpha) addp(pl.id, pl.alpha, fb == maskFb() ? maskPlaneAlpha(maskPct) : 0xffff);
			// 3F: the display waits on the GPU's own writes to this scanout buffer, so the compositor does not
			// have to glFinish. The kernel takes its own reference; the caller closes the fd after the commit.
			if (ff >= 0) addp(pl.id, pl.in_fence_fd, (uint64_t)(int64_t)ff);
		};
		addPlane(pn.primary, primaryFb, srcW, srcH, primaryFb != 0, fenceFd);
		addPlane(pn.overlay, overlayFb, SW, SH, overlayFb != 0, -1);
		if (pn.color.dirty)
		{
			addp(pn.crtc, pn.crtc_ctm, pn.color.ctm);
			addp(pn.crtc, pn.crtc_gamma_lut, pn.color.lut);
		}
		if (r.overflow)
		{
			fprintf(stderr, "kms: atomic request did not fit (%d properties in %d objects)\n", r.n, r.groups);
			return false;
		}
		// the same request drmModeAtomicCommit builds, without its allocation: count_objs is the number of object
		// groups, and props/vals are read through them in counts[] order
		drm_mode_atomic req = {};
		req.flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
		req.count_objs = (uint32_t)r.groups;
		req.objs_ptr = (uint64_t)(uintptr_t)r.objs;
		req.count_props_ptr = (uint64_t)(uintptr_t)r.counts;
		req.props_ptr = (uint64_t)(uintptr_t)r.props;
		req.prop_values_ptr = (uint64_t)(uintptr_t)r.vals;
		req.user_data = (uint64_t)(uintptr_t)&pending;
		int ret = drmIoctl(fd, DRM_IOCTL_MODE_ATOMIC, &req);
		if (ret)
		{
			fprintf(stderr, "kms: atomic commit failed: ret=%d errno=%s\n", ret, strerror(errno));
			return false;
		}
		pn.color.dirty = false;
		for (int i = 0; i < 2; i++)
		{
			if (overlayFb && overlayFb == pn.mask[i].fb) pn.maskShown = i;
			if (overlayFb && overlayFb == pn.frame[i].fb) pn.frameShown = i;
		}
		pending++;
		return true;
	}

	bool Display::presentGame(uint32_t fb, uint32_t w, uint32_t h, uint32_t overlayFb, int fenceFd)
	{
		return commitFrame(panel, fb, w, h, overlayFb, fenceFd);
	}

	bool Display::overlayCardBegin()
	{
		if (!panel.atomic) return false;
		Buf& b = panel.card[panel.cardFront ^ 1];	// the hidden buffer, never one the plane may scan
		if (!b.map) return false;
		// the composed base under the card, or transparent: the client on the primary shows through
		if (baseStale) shaderRecompose();
		if (frameCopy.size() == b.size) memcpy(b.map, frameCopy.data(), b.size);
		else memset(b.map, 0, b.size);
		return true;
	}

	void Display::overlayCardTurn(const uint32_t* canvas, int x0, int y0, int x1, int y1)
	{
		if (!panel.atomic || !canvas) return;
		x0 = std::max(0, x0); y0 = std::max(0, y0);
		x1 = std::min(W, x1); y1 = std::min(H, y1);
		if (x1 <= x0 || y1 <= y0) return;
		int back = panel.cardFront ^ 1;
		turn(canvas, panel.card[back], Damage{ x0, y0, x1, y1 });
		panel.cardFront = back;
	}

	uint32_t Display::overlayCardFb() const
	{
		return panel.atomic ? panel.card[panel.cardFront].fb : 0;
	}

	bool Display::frameSet(const uint32_t* canvas)
	{
		if (!panel.atomic) return false;
		Panel& pn = panel;
		if (!canvas)
		{
			pn.frameOn = false;
			std::vector<uint8_t>().swap(frameRaw);
			shaderRecompose();
			return true;
		}
		int back = pn.frameFront == pn.frameShown ? pn.frameFront ^ 1 : pn.frameFront;	// never the buffer the plane may scan
		Buf& b = pn.frame[back];
		if (!b.map && !makeBuf(b, DRM_FORMAT_ARGB8888)) return false;
		frameRaw.resize(b.size);
		Buf cached = b;
		cached.map = frameRaw.data();
		turn(canvas, cached, Damage{ 0, 0, W, H });
		pn.frameOn = true;
		shaderRecompose();
		memcpy(b.map, frameCopy.data(), b.size);
		pn.frameFront = back;
		return true;
	}

	// frameCopy from what is set now. The frame goes over the mask, so the mask only shows through the frame's hole.
	void Display::shaderRecompose()
	{
		baseStale = false;
		bool mask = maskMode != SHADER_MASK_OFF && !maskCopy.empty();
		bool frame = panel.frameOn && !frameRaw.empty();
		if (!mask && !frame) { std::vector<uint8_t>().swap(frameCopy); return; }
		if (!mask) { frameCopy = frameRaw; return; }
		frameCopy.resize(maskCopy.size());
		maskCompose((uint32_t*)frameCopy.data(), frame ? (const uint32_t*)frameRaw.data() : nullptr,
		            (const uint32_t*)maskCopy.data(), maskCopy.size() / 4, maskPct);
	}

	// the mask changed under a frame on screen: bake it into the hidden frame buffer, for frameFb() to name
	void Display::shaderRebakeFrame()
	{
		shaderRecompose();
		Panel& pn = panel;
		if (!pn.frameOn || frameCopy.empty()) return;
		int back = pn.frameFront == pn.frameShown ? pn.frameFront ^ 1 : pn.frameFront;
		Buf& b = pn.frame[back];
		if ((!b.map && !makeBuf(b, DRM_FORMAT_ARGB8888)) || b.size != frameCopy.size()) return;
		memcpy(b.map, frameCopy.data(), b.size);
		pn.frameFront = back;
	}

	bool Display::shaderSetMask(ShaderMask m, const MaskGeom& g)
	{
		if (fd < 0 || !panel.atomic || m >= SHADER_MASK_COUNT) return false;
		Panel& pn = panel;
		if (m == SHADER_MASK_OFF)
		{
			maskMode = m;
			std::vector<uint8_t>().swap(maskCopy);
			shaderRebakeFrame();
			return true;
		}
		int back = pn.maskFront == pn.maskShown ? pn.maskFront ^ 1 : pn.maskFront;	// never the buffer the plane may scan
		Buf& b = pn.mask[back];
		if (!b.map && !makeBuf(b, DRM_FORMAT_ARGB8888)) return false;
		maskCopy.resize(b.size);
		maskFill((uint32_t*)maskCopy.data(), b.pitch / 4, W, H, rot, m, g);
		memcpy(b.map, maskCopy.data(), b.size);
		pn.maskFront = back;
		maskMode = m;
		shaderRebakeFrame();
		return true;
	}

	void Display::shaderSetIntensity(int pct)
	{
		maskPct = std::min(100, std::max(0, pct));
		baseStale = true;
	}

	uint32_t Display::frameFb() const
	{
		return panel.atomic && panel.frameOn ? panel.frame[panel.frameFront].fb : 0;
	}

	bool Display::showMenu()
	{
		return commitMenu(panel, panel.buf[panel.front].fb);
	}

	// the menu on the overlay above a black primary, or on the primary under the mask
	bool Display::commitMenu(Panel& pn, uint32_t menuFb)
	{
		uint32_t m = maskFb();
		return m ? commitFrame(pn, menuFb, SW, SH, m) : commitFrame(pn, pn.black.fb, SW, SH, menuFb);
	}

	bool Display::blank()
	{
		return commitFrame(panel, panel.black.fb, SW, SH, 0);
	}

	void Display::handleEvents()
	{
		if (fd < 0) return;
		drmEventContext ev = {};
		ev.version = 2;
		ev.page_flip_handler = flipDone;
		drmHandleEvent(fd, &ev);
	}
}
