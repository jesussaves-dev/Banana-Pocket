#include "kms.h"
#include "turn.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
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
				if (pl.has_alpha) addp(pl.id, pl.alpha, 0xffff);	// the menu pixels are opaque
				if (pl.has_blend) addp(pl.id, pl.blend, 0);	// premultiplied
			}
		};
		addp(pn.conn, propId(pn.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), pn.crtc);
		addp(pn.crtc, propId(pn.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), pn.mode_blob);
		addp(pn.crtc, propId(pn.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
		addPlane(pn.primary, pn.black.fb, false);
		addPlane(pn.overlay, pn.buf[pn.front].fb, true);
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
			count = 1;
			rot = panelRotation(f, found[0].conn);
			SW = found[0].mode.hdisplay; SH = found[0].mode.vdisplay;
			// a panel mounted turned: the menu draws the other way round (1240x1080 on a 1080x1240 panel)
			W = rot == 90 || rot == 270 ? SH : SW;
			H = rot == 90 || rot == 270 ? SW : SH;
			p[0] = Panel();
			p[0].conn = found[0].conn; p[0].crtc = found[0].crtc;
			memcpy(p[0].mode, &found[0].mode, sizeof p[0].mode);

			// The menu belongs on the overlay plane when the driver exposes one and accepts atomic KMS: the primary
			// stays black, the overlay carries the ARGB menu (alpha 255, premultiplied, zpos top). Discovered by type
			// and possible_crtcs at runtime; a driver without the pair keeps the old single-plane path.
			bool atomic = drmSetClientCap(f, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
			              drmSetClientCap(f, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
			if (atomic) atomic = findPlanes(p[0], found[0].crtc_index);
			p[0].atomic = atomic;
			if (atomic)
			{
				if (drmModeCreatePropertyBlob(f, &p[0].mode, sizeof p[0].mode, &p[0].mode_blob) ||
				    !makeBuf(p[0].buf[0], DRM_FORMAT_ARGB8888) || !makeBuf(p[0].buf[1], DRM_FORMAT_ARGB8888) ||
				    !makeBuf(p[0].black, DRM_FORMAT_XRGB8888) || !makeBuf(p[0].card, DRM_FORMAT_ARGB8888))
				{
					p[0].atomic = false;
					if (p[0].mode_blob) { drmModeDestroyPropertyBlob(f, p[0].mode_blob); p[0].mode_blob = 0; }
					freeBuf(p[0].black);
					freeBuf(p[0].buf[0]); freeBuf(p[0].buf[1]); freeBuf(p[0].card);
				}
			}
			if (!p[0].atomic)
			{
				// the old path: one XRGB menu on the primary (QEMU's virtio, a driver without overlay/atomic)
				if (!makeBuf(p[0].buf[0], DRM_FORMAT_XRGB8888) || !makeBuf(p[0].buf[1], DRM_FORMAT_XRGB8888))
				{
					err = "can't make scanout buffers";
					close();
					return false;
				}
			}
			if (!restore())
			{
				if (p[0].atomic)
				{
					// atomic was advertised but rejected: remake the menu as XRGB and use the primary
					p[0].atomic = false;
					if (p[0].mode_blob) { drmModeDestroyPropertyBlob(f, p[0].mode_blob); p[0].mode_blob = 0; }
					freeBuf(p[0].black);
					freeBuf(p[0].buf[0]); freeBuf(p[0].buf[1]);
					if (!makeBuf(p[0].buf[0], DRM_FORMAT_XRGB8888) || !makeBuf(p[0].buf[1], DRM_FORMAT_XRGB8888) || !restore())
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
			        W, H, SW, SH, rot, p[0].atomic, p[0].primary.id, p[0].overlay.id);
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
		for (int s = 0; s < count; s++)
		{
			if (p[s].atomic) { if (!atomicModeset(p[s])) ok = false; }
			else if (drmModeSetCrtc(fd, p[s].crtc, p[s].buf[p[s].front].fb, 0, 0, &p[s].conn, 1, (drmModeModeInfo*)p[s].mode) != 0)
				ok = false;
		}
		if (ok) { pending = 0; applyGamma(); }
		return ok;
	}

	void Display::setWarmth(int level)
	{
		warmth = level;
		applyGamma();
	}

	// the night light: a linear ramp with green and blue pulled down, on every CRTC. Level 0 is the plain ramp, so
	// turning it off restores the panel's own look; the kernel keeps the LUT while a game's compositor has the panel.
	void Display::applyGamma()
	{
		if (fd < 0) return;
		double kg = warmth >= 2 ? 0.85 : warmth == 1 ? 0.92 : 1.0;
		double kb = warmth >= 2 ? 0.65 : warmth == 1 ? 0.82 : 1.0;
		for (int s = 0; s < count; s++)
		{
			drmModeCrtc* c = drmModeGetCrtc(fd, p[s].crtc);
			if (!c) continue;
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
				(void)drmModeCrtcSetGamma(fd, p[s].crtc, n, r.data(), g.data(), b.data());
			}
			drmModeFreeCrtc(c);
		}
	}

	void Display::close()
	{
		if (fd < 0) return;
		for (auto& pn : p)
		{
			freeBuf(pn.buf[0]); freeBuf(pn.buf[1]); freeBuf(pn.black); freeBuf(pn.card);
			if (pn.mode_blob) { drmModeDestroyPropertyBlob(fd, pn.mode_blob); pn.mode_blob = 0; }
		}
		::close(fd);
		fd = -1;
		pending = 0;
		rot = 0; SW = SH = 0;
	}

	bool Display::present(const uint32_t* const screens[2], const Damage dmg[2])
	{
		if (fd < 0 || pending) return false;
		bool any = false;
		for (int s = 0; s < count; s++)
		{
			Panel& pn = p[s];
			// the hidden buffer missed what changed since it was shown: this frame's part and the last frame's
			int back = pn.front ^ 1;
			Damage d = dmg[s];
			d.add(pn.missed[back]);
			pn.missed[back] = Damage();
			d.x0 = std::max(d.x0, 0); d.y0 = std::max(d.y0, 0);
			d.x1 = std::min(d.x1, W); d.y1 = std::min(d.y1, H);
			if (d.empty()) continue;
			Buf& bf = pn.buf[back];
			if (rot)
				turn(screens[s], bf, d);
			else if (d.x0 == 0 && d.x1 == W && bf.pitch == (uint32_t)W * 4)
			{
				// a full-width change on an unpadded buffer is one contiguous copy, not a row at a time
				memcpy(bf.map + (size_t)d.y0 * bf.pitch, screens[s] + (size_t)d.y0 * W, (size_t)(d.y1 - d.y0) * W * 4);
			}
			else
			{
				// only the columns that changed: the scanout buffer is uncached, every byte written costs
				size_t n = (size_t)(d.x1 - d.x0) * 4;
				for (int y = d.y0; y < d.y1; y++)
					memcpy(bf.map + (size_t)y * bf.pitch + (size_t)d.x0 * 4, screens[s] + (size_t)y * W + d.x0, n);
			}
			pn.missed[pn.front] = dmg[s];	// the buffer going hidden misses this frame
			if (pn.atomic)
			{
				if (commitFrame(pn, pn.black.fb, SW, SH, bf.fb))
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
		}
		return any;
	}

	// One atomic frame on a panel: the primary (a game buffer or the black one) and the overlay (the menu, or 0 to
	// hide it). srcW/srcH are the source size in pixels; the destination is always the panel mode, so a 1240x1080
	// client buffer is scaled to the 1080x1240 panel here (Phase 3B; 3C rotates it in EGL instead). The 3A menu
	// path passes the panel size for both and is pixel-identical to before.
	bool Display::commitFrame(Panel& pn, uint32_t primaryFb, uint32_t srcW, uint32_t srcH, uint32_t overlayFb)
	{
		if (fd < 0 || pending || !pn.atomic) return false;
		drmModeAtomicReq* r = drmModeAtomicAlloc();
		if (!r) return false;
		auto addp = [&](uint32_t obj, uint32_t prop, uint64_t v) { if (prop) drmModeAtomicAddProperty(r, obj, prop, v); };
		auto addPlane = [&](const Plane& pl, uint32_t fb, uint32_t sw, uint32_t sh, bool on) {
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
		};
		addPlane(pn.primary, primaryFb, srcW, srcH, primaryFb != 0);
		addPlane(pn.overlay, overlayFb, SW, SH, overlayFb != 0);
		int ret = drmModeAtomicCommit(fd, r, DRM_MODE_PAGE_FLIP_EVENT, &pending);
		drmModeAtomicFree(r);
		if (ret) return false;
		pending++;
		return true;
	}

	bool Display::presentGame(uint32_t fb, uint32_t w, uint32_t h, uint32_t overlayFb)
	{
		return commitFrame(p[0], fb, w, h, overlayFb);
	}

	bool Display::overlayCardBegin()
	{
		if (count <= 0 || !p[0].atomic || !p[0].card.map) return false;
		memset(p[0].card.map, 0, p[0].card.size);	// transparent: the client on the primary shows through
		return true;
	}

	void Display::overlayCardFill(int x, int y, int w, int h, uint32_t argb)
	{
		if (count <= 0 || !p[0].card.map) return;
		int x0 = std::max(0, x), y0 = std::max(0, y);
		int x1 = std::min<int>(SW, x + w), y1 = std::min<int>(SH, y + h);
		if (x1 <= x0 || y1 <= y0) return;
		// premultiplied, like the plane's blend mode
		uint32_t a = (argb >> 24) & 0xff;
		uint32_t r = ((argb >> 16) & 0xff) * a / 255;
		uint32_t g = ((argb >> 8) & 0xff) * a / 255;
		uint32_t b = (argb & 0xff) * a / 255;
		uint32_t px = (a << 24) | (r << 16) | (g << 8) | b;
		for (int yy = y0; yy < y1; yy++)
		{
			uint32_t* row = (uint32_t*)(p[0].card.map + (size_t)yy * p[0].card.pitch);
			for (int xx = x0; xx < x1; xx++) row[xx] = px;
		}
	}

	uint32_t Display::overlayCardFb() const
	{
		return count > 0 && p[0].atomic ? p[0].card.fb : 0;
	}

	bool Display::showMenu()
	{
		return commitFrame(p[0], p[0].black.fb, SW, SH, p[0].buf[p[0].front].fb);
	}

	bool Display::blank()
	{
		return commitFrame(p[0], p[0].black.fb, SW, SH, 0);
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
