// Phase 3B: the in-process Wayland server. One kiosk client, one fullscreen xdg toplevel, linear DMA-BUFs only,
// presented on the DRM primary plane with the menu overlay hidden. No rotation here (3C): the client's 1240x1080
// buffer is scaled to the 1080x1240 panel by the plane, so the picture is sideways on purpose until the EGL pass.
//
// The server is a peer of the menu loop: its event loop fd joins the existing poll(), and its frames go through
// the same one-flip-in-flight gate in Display. A client buffer is imported once (drmPrimeFDToHandle + AddFB2 with
// LINEAR), kept alive by the compositor while it is on screen, and released with wl_buffer.release after the page
// flip event. A disconnect puts the menu back.
#include "compositor.h"
#include "client_buffer.h"
#include "dmabuf.h"
#include "egl_presenter.h"
#include "presenter.h"
#include "trace.h"
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <drm_fourcc.h>
#include <wayland-server.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "wayland-drm-server-protocol.h"

namespace banana
{
	namespace
	{
		constexpr const char* kWaylandDir = "/run/banana/wayland";
		constexpr const char* kSocket = "banana-0";
		constexpr int kClientFormatW = 1240;	// the measured client buffer / the menu canvas
		constexpr int kClientFormatH = 1080;

		void logf(const char* fmt, ...)
		{
			va_list ap;
			va_start(ap, fmt);
			fputs("compositor: ", stderr);
			vfprintf(stderr, fmt, ap);
			fputc('\n', stderr);
			va_end(ap);
		}
	}

	struct Compositor::Impl
	{
		using Buffer = ClientBuffer;	// wl_buffer resource + import parameters + per-presenter caches
		struct Surface
		{
			Impl* im = nullptr;
			wl_resource* res = nullptr;	// wl_surface
			wl_resource* xdg = nullptr;	// xdg_surface
			wl_resource* top = nullptr;	// xdg_toplevel
			bool configured = false;
			Buffer* pend = nullptr;		// attached, not committed
		};

		Display* d = nullptr;
		wl_display* wl = nullptr;
		wl_event_loop* loop = nullptr;
		wl_client* client = nullptr;
		wl_listener clientCreated = {};
		wl_listener clientGone = {};
		std::vector<Surface*> surfaces;	// every wl_surface the client made; only one may present
		Surface* surf = nullptr;		// the one presenting (the first that commits a buffer)

		Buffer* pending = nullptr;		// committed, not yet presented
		Buffer* inflight = nullptr;		// presented, flip not completed
		Buffer* onScreen = nullptr;		// the FB the primary is scanning now (kept until replaced)
		wl_resource* pendCb = nullptr;		// a frame callback requested for the next commit
		wl_resource* pendingCb = nullptr;
		wl_resource* inflightCb = nullptr;
		wl_listener pendCbGone = {};
		wl_listener pendingCbGone = {};
		wl_listener inflightCbGone = {};

		bool hidMenu = false;			// presentGame() is on screen and the overlay is off
		uint32_t overlayFb = 0;			// the 3D card buffer (ARGB), shown above the client when non-zero
		uint32_t lastFb = 0, lastW = 0, lastH = 0;	// the primary currently on screen, for an overlay-only commit
		bool restoreMenu = false;		// show the menu as soon as the current flip completes
		bool stopping = false;
		bool saidShm = false;
		bool saidSecondary = false;
		unsigned frames = 0;
		uint64_t nextBufferSerial = 0;
		static constexpr unsigned kTraceFrames = 16;	// durable per-frame trace budget
		static constexpr unsigned kTraceSerials = 16;	// per-buffer lifecycle is bounded by serial, not by frame
		std::unique_ptr<Presenter> presenter;	// EGL rotate (3C) or the direct 3B fallback
		dev_t dev = 0;
		char devName[128] = "";

		struct Adv { uint32_t format; uint64_t modifier; } adv[2] = {
			{ DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR },
			{ DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR },
		};
		int nadv = 2;

		void bufferFree(Buffer* b)
		{
			if (b->serial <= kTraceSerials)
				tracef("buffer free serial=%llu fd=%d", (unsigned long long)b->serial, b->fd);
			// the presenter owns the imported/EGL objects; it is still alive here (clients are destroyed before
			// presenter->stop()), so the context and DRM fd are valid
			if (presenter) presenter->forget(*b);
			if (b->fd >= 0) close(b->fd);
			delete b;
		}
		void bufferDrop(Buffer* b) { if (--b->refs <= 0) bufferFree(b); }
		void bufferRelease(Buffer* b)	// after the frame is no longer in use
		{
			if (b->res) wl_buffer_send_release(b->res);
			bufferDrop(b);
		}
		void dropPending()
		{
			if (!pending) return;
			Buffer* b = pending;
			pending = nullptr;
			bufferRelease(b);
		}
		void putPendingCb(wl_resource* cb)
		{
			if (pendingCb) wl_resource_destroy(pendingCb);	// its destroy handler nulls the slot and unlinks
			pendingCb = cb;
			if (cb) wl_resource_add_destroy_listener(cb, &pendingCbGone);
		}
		void fireCb(wl_resource*& slot, wl_listener& l)
		{
			if (!slot) return;
			wl_resource* cb = slot;
			slot = nullptr;
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			wl_callback_send_done(cb, (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000));
			wl_resource_destroy(cb);
		}
		void frame()
		{
			if (!pending || d->flipPending()) return;
			Buffer* b = pending;
			uint32_t fb = 0;
			std::string err;
			if ((!presenter || !presenter->diagnostic()) && frames < kTraceFrames)
				tracef("frame: render begin serial=%llu res=%u %ux%u", (unsigned long long)b->serial,
				       b->res ? wl_resource_get_id(b->res) : 0, b->w, b->h);
			// The EGL presenter is panel-sized: a source that is not the 1240x1080 logical output cannot be
			// rotated by it. Fall back to the direct presenter (3B: plane-scaled and sideways) only while no
			// presenter-owned frame is on screen; a mid-session size change with a frame up is dropped.
			if (presenter && presenter->rotated() && ((int)b->w != d->width() || (int)b->h != d->height()))
			{
				if (!d->flipPending() && !inflight && !presenter->scanning())
				{
					logf("source %ux%u is not %dx%d: switching to the direct presenter", b->w, b->h, d->width(), d->height());
					presenter->stop();
					auto direct = std::make_unique<DirectPresenter>(*d);
					std::string derr;
					if (!direct->start(derr))
					{
						logf("direct presenter unavailable: %s", derr.c_str());
						presenter.reset();
						dropPending();
						return;
					}
					presenter = std::move(direct);
				}
				else
				{
					logf("source %ux%u cannot be rotated while a frame is on screen; dropping it", b->w, b->h);
					dropPending();
					return;
				}
			}
			if (!presenter || !presenter->render(*b, fb, err))
			{
				logf("render: %s", err.empty() ? "no presenter" : err.c_str());
				if (presenter) presenter->abandon();
				dropPending();
				return;
			}
			if (presenter->diagnostic())
			{
				dropPending();	// the isolation operation and its glFinish are done; no KMS submission
				return;
			}
			// the EGL path produces the panel-sized scanout buffer (1:1); the direct fallback passes the client size
			uint32_t pw = presenter->rotated() ? (uint32_t)d->panelWidth() : b->w;
			uint32_t ph = presenter->rotated() ? (uint32_t)d->panelHeight() : b->h;
			if (frames < 3) logf("commit: fb=%u src=%ux%u", fb, pw, ph);
			if (frames < kTraceFrames)
				tracef("frame: commit serial=%llu fb=%u %ux%u", (unsigned long long)b->serial, fb, pw, ph);
			if (!d->presentGame(fb, pw, ph, overlayFb))
			{
				presenter->abandon();
				if (!d->flipPending())
				{
					logf("presenting a client frame failed; dropped it");
					dropPending();
				}
				return;
			}
			presenter->presented();
			lastFb = fb;
			lastW = pw;
			lastH = ph;
			if (frames < kTraceFrames)
				tracef("frame: presented serial=%llu fb=%u", (unsigned long long)b->serial, fb);
			pending = nullptr;
			inflight = b;
			hidMenu = true;
			if (!presenter->rotated())
			{
				// the direct path scans the client's own framebuffer: keep it referenced until the next commit
				if (onScreen != b)
				{
					if (onScreen) bufferDrop(onScreen);
					b->refs++;
					onScreen = b;
				}
			}
			else if (onScreen)
			{
				bufferDrop(onScreen);	// EGL: the source is not the scanout, the flip reference above is enough
				onScreen = nullptr;
			}
			if (frames++ < 3 || frames % 60 == 0) logf("present %ux%u fb=%u (frame %u)", b->w, b->h, fb, frames);
			if (pendingCb)
			{
				wl_resource* cb = pendingCb;
				pendingCb = nullptr;
				wl_list_remove(&pendingCbGone.link);
				// the callback becomes the in-flight one
				inflightCb = cb;
				wl_resource_add_destroy_listener(cb, &inflightCbGone);
			}
		}
		void vblank()
		{
			if (inflight && !d->flipPending())
			{
				Buffer* b = inflight;
				inflight = nullptr;
				bufferRelease(b);
				fireCb(inflightCb, inflightCbGone);
			}
			if (presenter && !d->flipPending()) presenter->flipDone();
			if (restoreMenu && !inflight && !d->flipPending())
			{
				restoreMenu = false;
				showMenu();
			}
			if (!restoreMenu) frame();
		}
		void surfaceGone(Surface* s)
		{
			s->res = nullptr;
			if (s->pend) { bufferRelease(s->pend); s->pend = nullptr; }
			s->xdg = s->top = nullptr;	// the client's resources are going away too
			if (surf == s) surf = nullptr;
			for (size_t i = 0; i < surfaces.size(); i++)
				if (surfaces[i] == s) { surfaces.erase(surfaces.begin() + i); break; }
			delete s;
		}
		void showMenu()
		{
			if (stopping) return;
			if (d->showMenu())
			{
				hidMenu = false;
				lastFb = 0;	// the menu owns the primary again: no overlay-only commits over it
				if (presenter) presenter->detached();
				if (onScreen) { bufferDrop(onScreen); onScreen = nullptr; }
			}
		}
		void clientDisconnected()
		{
			tracef("client disconnected");
			client = nullptr;
			if (pendCb) { pendCb = nullptr; }		// their resource destroy handlers unlink the listeners
			if (pendingCb) { pendingCb = nullptr; }
			// the Surface objects are freed by their wl_surface resource destroy callback (surfaceGone), which runs
			// after this: wl_client_destroy emits the client destroy signal before it destroys the resources
			surf = nullptr;
			dropPending();
			if (inflight) restoreMenu = true;
			else if (hidMenu && !stopping) showMenu();
			logf("client disconnected");
		}
	};

	namespace
	{
		using Impl = Compositor::Impl;

		// ---- buffers ------------------------------------------------------------------------------------------------

		void bufferResourceGone(wl_resource* res)
		{
			auto* b = (Impl::Buffer*)wl_resource_get_user_data(res);
			if (!b) return;
			b->res = nullptr;
			((Impl*)b->im)->bufferDrop(b);
		}
		void bufferDestroy(wl_client*, wl_resource* res) { wl_resource_destroy(res); }
		const struct wl_buffer_interface bufferImpl = { .destroy = bufferDestroy };

		void cbGonePending(wl_listener* l, void* data)
		{
			Impl* im = wl_container_of(l, im, pendingCbGone);
			if ((wl_resource*)data == im->pendingCb) im->pendingCb = nullptr;
			wl_list_remove(&l->link);
		}
		void cbGoneInflight(wl_listener* l, void* data)
		{
			Impl* im = wl_container_of(l, im, inflightCbGone);
			if ((wl_resource*)data == im->inflightCb) im->inflightCb = nullptr;
			wl_list_remove(&l->link);
		}
		void cbGonePend(wl_listener* l, void* data)
		{
			Impl* im = wl_container_of(l, im, pendCbGone);
			if ((wl_resource*)data == im->pendCb) im->pendCb = nullptr;
			wl_list_remove(&l->link);
		}

		// ---- xdg-shell ----------------------------------------------------------------------------------------------

		void xdgTopDestroy(wl_client*, wl_resource* res)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			if (s && s->top == res) s->top = nullptr;
			wl_resource_destroy(res);
		}
		void xdgTopNoop2(wl_client*, wl_resource* r, wl_resource*) { (void)r; }
		void xdgTopNoopStr(wl_client*, wl_resource* r, const char*) { (void)r; }
		void xdgTopNoopSeat(wl_client*, wl_resource* r, wl_resource*, uint32_t, int32_t, int32_t) { (void)r; }
		void xdgTopMove(wl_client*, wl_resource* r, wl_resource*, uint32_t) { (void)r; }
		void xdgTopResize(wl_client*, wl_resource* r, wl_resource*, uint32_t, uint32_t) { (void)r; }
		void xdgTop2i(wl_client*, wl_resource* r, int32_t, int32_t) { (void)r; }
		void xdgTopNoop0(wl_client*, wl_resource* r) { (void)r; }
		const struct xdg_toplevel_interface toplevelImpl = {
			.destroy = xdgTopDestroy, .set_parent = xdgTopNoop2, .set_title = xdgTopNoopStr, .set_app_id = xdgTopNoopStr,
			.show_window_menu = xdgTopNoopSeat, .move = xdgTopMove, .resize = xdgTopResize,
			.set_max_size = xdgTop2i, .set_min_size = xdgTop2i,
			.set_maximized = xdgTopNoop0, .unset_maximized = xdgTopNoop0,
			.set_fullscreen = xdgTopNoop2, .unset_fullscreen = xdgTopNoop0, .set_minimized = xdgTopNoop0,
		};

		void xdgSurfDestroy(wl_client*, wl_resource* res)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			if (s && s->xdg == res) s->xdg = nullptr;
			wl_resource_destroy(res);
		}
		void xdgSurfToplevel(wl_client* c, wl_resource* res, uint32_t id)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			if (!s) return;
			if (s->top) { wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "a second toplevel"); return; }
			s->top = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(s->top, &toplevelImpl, s, nullptr);
		}
		void xdgSurfPopup(wl_client*, wl_resource* res, uint32_t, wl_resource*, wl_resource*)
		{
			wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "popups are not supported");
		}
		void xdgSurfGeom(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		void xdgSurfAck(wl_client*, wl_resource*, uint32_t) {}
		const struct xdg_surface_interface xdgSurfaceImpl = {
			.destroy = xdgSurfDestroy, .get_toplevel = xdgSurfToplevel, .get_popup = xdgSurfPopup,
			.set_window_geometry = xdgSurfGeom, .ack_configure = xdgSurfAck,
		};

		void xdgPosDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		void xdgPosSize(wl_client*, wl_resource*, int32_t, int32_t) {}
		void xdgPosRect(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		void xdgPosAnchor(wl_client*, wl_resource*, uint32_t) {}
		void xdgPosGravity(wl_client*, wl_resource*, uint32_t) {}
		void xdgPosAdjust(wl_client*, wl_resource*, uint32_t) {}
		void xdgPosOffset(wl_client*, wl_resource*, int32_t, int32_t) {}
		void xdgPosReactive(wl_client*, wl_resource*) {}
		void xdgPosParentSize(wl_client*, wl_resource*, int32_t, int32_t) {}
		void xdgPosParentCfg(wl_client*, wl_resource*, uint32_t) {}
		const struct xdg_positioner_interface positionerImpl = {
			.destroy = xdgPosDestroy, .set_size = xdgPosSize, .set_anchor_rect = xdgPosRect,
			.set_anchor = xdgPosAnchor, .set_gravity = xdgPosGravity, .set_constraint_adjustment = xdgPosAdjust,
			.set_offset = xdgPosOffset, .set_reactive = xdgPosReactive,
			.set_parent_size = xdgPosParentSize, .set_parent_configure = xdgPosParentCfg,
		};

		void xdgWmDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		void xdgWmPositioner(wl_client* c, wl_resource* res, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(r, &positionerImpl, nullptr, nullptr);
		}
		void xdgWmGetSurface(wl_client* c, wl_resource* res, uint32_t id, wl_resource* surface)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(surface);
			if (!s) return;
			if (s->xdg) { wl_resource_post_error(res, XDG_WM_BASE_ERROR_ROLE, "a second xdg_surface"); return; }
			s->xdg = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(s->xdg, &xdgSurfaceImpl, s, nullptr);
		}
		void xdgWmPong(wl_client*, wl_resource*, uint32_t) {}
		const struct xdg_wm_base_interface xdgWmImpl = {
			.destroy = xdgWmDestroy, .create_positioner = xdgWmPositioner,
			.get_xdg_surface = xdgWmGetSurface, .pong = xdgWmPong,
		};

		void xdgBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &xdg_wm_base_interface, version < 1 ? version : 1, id);
			wl_resource_set_implementation(r, &xdgWmImpl, data, nullptr);
		}

		// ---- wl_surface ---------------------------------------------------------------------------------------------

		void surfDestroy(wl_client*, wl_resource* res) { wl_resource_destroy(res); }
		void surfAttach(wl_client*, wl_resource* res, wl_resource* buf, int32_t, int32_t)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			Impl* im = s->im;
			Impl::Buffer* b = nullptr;
			if (buf)
			{
				if (wl_shm_buffer_get(buf))
				{
					if (!im->saidShm) { im->saidShm = true; logf("shm frame ignored (linux-dmabuf only)"); }
				}
				else
				{
					b = (Impl::Buffer*)wl_resource_get_user_data(buf);
					if (b) b->refs++;
				}
			}
			if (s->pend) im->bufferRelease(s->pend);
			s->pend = b;
		}
		void surfDamage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		void surfFrame(wl_client* c, wl_resource* res, uint32_t id)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			Impl* im = s->im;
			wl_resource* cb = wl_resource_create(c, &wl_callback_interface, wl_resource_get_version(res), id);
			if (im->pendCb) wl_resource_destroy(im->pendCb);	// its destroy handler nulls the slot and unlinks
			im->pendCb = cb;
			wl_resource_add_destroy_listener(cb, &im->pendCbGone);
		}
		void surfRegion(wl_client*, wl_resource*, wl_resource*) {}
		void surfCommit(wl_client*, wl_resource* res)
		{
			auto* s = (Impl::Surface*)wl_resource_get_user_data(res);
			Impl* im = s->im;
			if (!s->configured)
			{
				// the first commit after the role is assigned: answer with one fullscreen configure (a commit
				// before the role exists only sets up the surface; keep waiting for the one that asks)
				if (s->top && s->xdg)
				{
					struct wl_array states;
					wl_array_init(&states);
					if (uint32_t* st = (uint32_t*)wl_array_add(&states, sizeof *st)) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
					xdg_toplevel_send_configure(s->top, kClientFormatW, kClientFormatH, &states);
					xdg_surface_send_configure(s->xdg, wl_display_next_serial(im->wl));
					logf("configure %dx%d", kClientFormatW, kClientFormatH);
					wl_array_release(&states);
					s->configured = true;
				}
				return;
			}
			if (s->pend && im->surf && im->surf != s)
			{
				// a second surface's buffer: a kiosk shows the first surface that presents, not this one
				if (!im->saidSecondary) { im->saidSecondary = true; logf("a second surface committed a buffer; ignored"); }
				im->bufferRelease(s->pend);
				s->pend = nullptr;
				return;
			}
			if (s->pend)
			{
				if (!im->surf) im->surf = s;
				if (im->pending) im->dropPending();
				im->pending = s->pend;
				s->pend = nullptr;
				if (im->pendCb)
				{
					wl_resource* cb = im->pendCb;
					im->pendCb = nullptr;
					wl_list_remove(&im->pendCbGone.link);
					im->putPendingCb(cb);
				}
			}
			im->frame();
		}
		void surfTransform(wl_client*, wl_resource*, int32_t) {}
		void surfScale(wl_client*, wl_resource*, int32_t) {}
		void surfDamageBuffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		const struct wl_surface_interface surfaceImpl = {
			.destroy = surfDestroy, .attach = surfAttach, .damage = surfDamage, .frame = surfFrame,
			.set_opaque_region = surfRegion, .set_input_region = surfRegion, .commit = surfCommit,
			.set_buffer_transform = surfTransform, .set_buffer_scale = surfScale, .damage_buffer = surfDamageBuffer,
		};

		void regionDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		void regionAdd(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		void regionSub(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
		const struct wl_region_interface regionImpl = { .destroy = regionDestroy, .add = regionAdd, .subtract = regionSub };

		void compSurface(wl_client* c, wl_resource* res, uint32_t id)
		{
			Impl* im = (Impl*)wl_resource_get_user_data(res);
			auto* s = new Impl::Surface();
			s->im = im;
			s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(s->res, &surfaceImpl, s, [](wl_resource* r) {
				if (auto* s = (Impl::Surface*)wl_resource_get_user_data(r)) s->im->surfaceGone(s);
			});
			im->surfaces.push_back(s);
		}
		void compRegion(wl_client* c, wl_resource* res, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(r, &regionImpl, nullptr, nullptr);
		}
		const struct wl_compositor_interface compositorImpl = { .create_surface = compSurface, .create_region = compRegion };

		void compositorBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &wl_compositor_interface, version < 4 ? version : 4, id);
			wl_resource_set_implementation(r, &compositorImpl, data, nullptr);
		}

		// ---- output and seat ------------------------------------------------------------------------------------------

		void outputRelease(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		const struct wl_output_interface outputImpl = { .release = outputRelease };
		void outputBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			(void)data;
			wl_resource* r = wl_resource_create(c, &wl_output_interface, version < 3 ? version : 3, id);
			wl_resource_set_implementation(r, &outputImpl, nullptr, nullptr);
			wl_output_send_geometry(r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Banana", "Retroid Pocket Classic", WL_OUTPUT_TRANSFORM_NORMAL);
			wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, kClientFormatW, kClientFormatH, 60000);
			if (version >= 2) wl_output_send_scale(r, 1);
			if (version >= 2) wl_output_send_done(r);
		}

		void seatGetPtr(wl_client* c, wl_resource* res, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(res), id);
			static const struct wl_pointer_interface ptrImpl = {
				.set_cursor = [](wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {},
				.release = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
			};
			wl_resource_set_implementation(r, &ptrImpl, nullptr, nullptr);
		}
		void seatGetKb(wl_client* c, wl_resource* res, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(res), id);
			static const struct wl_keyboard_interface kbImpl = {
				.release = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
			};
			wl_resource_set_implementation(r, &kbImpl, nullptr, nullptr);
		}
		void seatGetTouch(wl_client* c, wl_resource* res, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(res), id);
			static const struct wl_touch_interface touchImpl = {
				.release = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
			};
			wl_resource_set_implementation(r, &touchImpl, nullptr, nullptr);
		}
		void seatRelease(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		const struct wl_seat_interface seatImpl = {
			.get_pointer = seatGetPtr, .get_keyboard = seatGetKb, .get_touch = seatGetTouch, .release = seatRelease,
		};
		void seatBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			(void)data;
			wl_resource* r = wl_resource_create(c, &wl_seat_interface, version < 5 ? version : 5, id);
			wl_resource_set_implementation(r, &seatImpl, nullptr, nullptr);
			wl_seat_send_capabilities(r, 0);	// input forwarding is Phase 3E
			if (version >= 2) wl_seat_send_name(r, "seat0");
		}

		// ---- wl_drm (Mesa's WSI asks for it; buffers still arrive through linux-dmabuf) -------------------------------

		void drmAuthenticate(wl_client*, wl_resource* res, uint32_t magic)
		{
			Impl* im = (Impl*)wl_resource_get_user_data(res);
			if (im->d->drmFd() >= 0 && drmAuthMagic(im->d->drmFd(), magic) == 0) wl_drm_send_authenticated(res);
			else wl_resource_post_error(res, 0, "authenticate failed");
		}
		void drmCreateBuffer(wl_client*, wl_resource* res, uint32_t, uint32_t, int32_t, int32_t, uint32_t, uint32_t)
		{
			wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
		}
		void drmCreatePlanar(wl_client*, wl_resource* res, uint32_t, uint32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
		{
			wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
		}
		void drmCreatePrime(wl_client*, wl_resource* res, uint32_t, int32_t, int32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)
		{
			wl_resource_post_error(res, 0, "GEM-name buffers are not supported; use linux-dmabuf");
		}
		const struct wl_drm_interface drmImpl = {
			.authenticate = drmAuthenticate, .create_buffer = drmCreateBuffer,
			.create_planar_buffer = drmCreatePlanar, .create_prime_buffer = drmCreatePrime,
		};
		void drmBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			Impl* im = (Impl*)data;
			wl_resource* r = wl_resource_create(c, &wl_drm_interface, version < 2 ? version : 2, id);
			wl_resource_set_implementation(r, &drmImpl, data, nullptr);
			wl_drm_send_device(r, im->devName);
			wl_drm_send_format(r, DRM_FORMAT_ARGB8888);
			wl_drm_send_format(r, DRM_FORMAT_XRGB8888);
			wl_drm_send_capabilities(r, WL_DRM_CAPABILITY_PRIME);
		}

		// ---- linux-dmabuf v4 ------------------------------------------------------------------------------------------

		void feedbackDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		const struct zwp_linux_dmabuf_feedback_v1_interface feedbackImpl = { .destroy = feedbackDestroy };
		void sendFeedback(wl_client* c, Impl* im, uint32_t id)
		{
			wl_resource* res = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
			if (!res) return;
			wl_resource_set_implementation(res, &feedbackImpl, nullptr, nullptr);
			struct entry { uint32_t format, padding; uint64_t modifier; } table[2];
			for (int i = 0; i < im->nadv; i++) { table[i].format = im->adv[i].format; table[i].padding = 0; table[i].modifier = im->adv[i].modifier; }
			int fd = memfd_create("banana-dmabuf-table", MFD_CLOEXEC);
			if (fd >= 0)
			{
				size_t sz = (size_t)im->nadv * sizeof table[0];
				if (write(fd, table, sz) == (ssize_t)sz) zwp_linux_dmabuf_feedback_v1_send_format_table(res, fd, (uint32_t)sz);
				close(fd);
			}
			struct wl_array dev;
			wl_array_init(&dev);
			if (void* p = wl_array_add(&dev, sizeof im->dev)) memcpy(p, &im->dev, sizeof im->dev);
			zwp_linux_dmabuf_feedback_v1_send_main_device(res, &dev);
			zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(res, &dev);
			wl_array_release(&dev);
			struct wl_array idx;
			wl_array_init(&idx);
			for (int i = 0; i < im->nadv; i++) if (uint16_t* p = (uint16_t*)wl_array_add(&idx, sizeof *p)) *p = (uint16_t)i;
			zwp_linux_dmabuf_feedback_v1_send_tranche_formats(res, &idx);
			wl_array_release(&idx);
			zwp_linux_dmabuf_feedback_v1_send_tranche_flags(res, 0);
			zwp_linux_dmabuf_feedback_v1_send_tranche_done(res);
			zwp_linux_dmabuf_feedback_v1_send_done(res);
		}

		void paramsResourceGone(wl_resource* res)
		{
			auto* b = (Impl::Buffer*)wl_resource_get_user_data(res);
			if (b && !b->res) ((Impl*)b->im)->bufferFree(b);	// a created buffer is owned by its wl_buffer resource
		}
		void paramsDestroy(wl_client*, wl_resource* res) { wl_resource_destroy(res); }
		void paramsAdd(wl_client*, wl_resource* res, int32_t fd, uint32_t plane, uint32_t off, uint32_t stride, uint32_t hi, uint32_t lo)
		{
			auto* b = (Impl::Buffer*)wl_resource_get_user_data(res);
			if (!b) return;
			if (plane != 0) { wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "one plane"); return; }
			if (b->fd >= 0) close(b->fd);
			b->fd = dup(fd);
			b->off = off;
			b->stride = stride;
			b->modifier = ((uint64_t)hi << 32) | lo;
		}
		bool paramsValidate(wl_resource* res, Impl::Buffer* b, int32_t w, int32_t h, uint32_t format)
		{
			// Any positive size is accepted: a source the EGL presenter cannot rotate (anything but the
			// panel's 1240x1080 logical output) makes Compositor::frame() switch to the direct presenter.
			if (w <= 0 || h <= 0)
			{
				logf("rejecting %dx%d: invalid dimensions", w, h);
				wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
				                       "invalid dimensions");
				return false;
			}
			if (!banana::dmabufSupported(format, b->modifier, b->fd >= 0 ? 1 : 0))
			{
				logf("rejecting a buffer: %dx%d fmt=%u mod=0x%016llx planes=%d", w, h, format,
				     (unsigned long long)b->modifier, b->fd >= 0 ? 1 : 0);
				wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				                       "only linear XRGB8888/ARGB8888, one plane");
				return false;
			}
			b->w = (uint32_t)w;
			b->h = (uint32_t)h;
			b->format = format;
			return true;
		}
		void paramsCreate(wl_client* c, wl_resource* res, int32_t w, int32_t h, uint32_t format, uint32_t)
		{
			auto* b = (Impl::Buffer*)wl_resource_get_user_data(res);
			if (!b || !paramsValidate(res, b, w, h, format)) return;
			wl_resource* buf = wl_resource_create(c, &wl_buffer_interface, 1, 0);
			if (!buf) return;
			b->res = buf;
			b->refs = 1;
			wl_resource_set_implementation(buf, &bufferImpl, b, bufferResourceGone);
			zwp_linux_buffer_params_v1_send_created(res, buf);
		}
		void paramsCreateImmed(wl_client* c, wl_resource* res, uint32_t id, int32_t w, int32_t h, uint32_t format, uint32_t)
		{
			auto* b = (Impl::Buffer*)wl_resource_get_user_data(res);
			if (!b || !paramsValidate(res, b, w, h, format)) return;
			wl_resource* buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
			if (!buf) return;
			b->res = buf;
			b->refs = 1;
			wl_resource_set_implementation(buf, &bufferImpl, b, bufferResourceGone);
		}
		const struct zwp_linux_buffer_params_v1_interface paramsImpl = {
			.destroy = paramsDestroy, .add = paramsAdd, .create = paramsCreate, .create_immed = paramsCreateImmed,
		};
		void dmabufDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
		void dmabufCreateParams(wl_client* c, wl_resource* res, uint32_t id)
		{
			Impl* im = (Impl*)wl_resource_get_user_data(res);
			auto* b = new Impl::Buffer();
			b->im = im;
			b->serial = ++im->nextBufferSerial;
			wl_resource* r = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(res), id);
			wl_resource_set_implementation(r, &paramsImpl, b, paramsResourceGone);
		}
		void dmabufDefaultFeedback(wl_client* c, wl_resource* res, uint32_t id)
		{
			sendFeedback(c, (Impl*)wl_resource_get_user_data(res), id);
		}
		void dmabufSurfaceFeedback(wl_client* c, wl_resource* res, uint32_t id, wl_resource*)
		{
			sendFeedback(c, (Impl*)wl_resource_get_user_data(res), id);
		}
		const struct zwp_linux_dmabuf_v1_interface dmabufImpl = {
			.destroy = dmabufDestroy, .create_params = dmabufCreateParams,
			.get_default_feedback = dmabufDefaultFeedback, .get_surface_feedback = dmabufSurfaceFeedback,
		};
		void dmabufBind(wl_client* c, void* data, uint32_t version, uint32_t id)
		{
			wl_resource* r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, version < 4 ? version : 4, id);
			wl_resource_set_implementation(r, &dmabufImpl, data, nullptr);
		}

		// ---- client lifecycle ----------------------------------------------------------------------------------------

		void onClientCreated(wl_listener* l, void* data)
		{
			Impl* im = wl_container_of(l, im, clientCreated);
			wl_client* c = (wl_client*)data;
			if (im->client)
			{
				logf("a second client is refused (one fullscreen client)");
				wl_client_destroy(c);
				return;
			}
			im->client = c;
			wl_client_add_destroy_listener(c, &im->clientGone);
			logf("client connected");
			tracef("client connected");
		}
		void onClientGone(wl_listener* l, void*)
		{
			Impl* im = wl_container_of(l, im, clientGone);
			im->clientDisconnected();
		}
	}

	// ---- public ----------------------------------------------------------------------------------------------------

	Compositor::~Compositor() { stop(); }

	bool Compositor::start(std::string& err)
	{
		if (impl) return true;
		if (!d.isOpen()) { err = "the panel is not open"; return false; }
		if (!d.atomic()) { err = "atomic KMS with an overlay plane is needed"; return false; }

		Impl* im = new Impl();
		im->d = &d;
		if (mkdir(kWaylandDir, 0700) && errno != EEXIST) { err = std::string("mkdir ") + kWaylandDir + ": " + strerror(errno); delete im; return false; }
		unlink((std::string(kWaylandDir) + "/" + kSocket).c_str());
		setenv("XDG_RUNTIME_DIR", kWaylandDir, 1);
		im->wl = wl_display_create();
		if (!im->wl) { err = "wl_display_create failed"; delete im; return false; }
		im->loop = wl_display_get_event_loop(im->wl);
		if (wl_display_add_socket(im->wl, kSocket))
		{
			err = std::string("wl_display_add_socket ") + kSocket + ": " + strerror(errno);
			wl_display_destroy(im->wl);
			delete im;
			return false;
		}
		char* name = drmGetDeviceNameFromFd2(d.drmFd());
		snprintf(im->devName, sizeof im->devName, "%s", name ? name : "/dev/dri/card0");
		free(name);
		struct stat st;
		if (fstat(d.drmFd(), &st) == 0) im->dev = st.st_rdev;

		im->clientCreated.notify = onClientCreated;
		im->clientGone.notify = onClientGone;
		im->pendCbGone.notify = cbGonePend;
		im->pendingCbGone.notify = cbGonePending;
		im->inflightCbGone.notify = cbGoneInflight;

		wl_display_add_client_created_listener(im->wl, &im->clientCreated);
		wl_global_create(im->wl, &wl_compositor_interface, 4, im, compositorBind);
		wl_global_create(im->wl, &wl_output_interface, 3, im, outputBind);
		wl_global_create(im->wl, &wl_seat_interface, 5, im, seatBind);
		wl_global_create(im->wl, &xdg_wm_base_interface, 1, im, xdgBind);
		wl_global_create(im->wl, &zwp_linux_dmabuf_v1_interface, 4, im, dmabufBind);
		wl_global_create(im->wl, &wl_drm_interface, 2, im, drmBind);
		wl_display_init_shm(im->wl);

		// Presentation strategy. 3C (the EGL rotate presenter) is the default; if it cannot start, the 3B direct
		// presenter keeps the client working and logs that the output is not rotated. A persistent
		// /data/banana/3c-direct forces the direct presenter (the escape hatch); /run/banana/3c-egl and
		// /run/banana/3c-trace only turn on the diagnostic trace.
		// Stage 8 isolation variants: one gate each, never combined. They imply the EGL presenter but the compositor
		// releases the frame instead of submitting it, so the operation is the only thing exercised.
		const bool diagImport = access("/run/banana/3c-diag-import", F_OK) == 0;
		const bool diagPbuffer = access("/run/banana/3c-diag-pbuffer", F_OK) == 0;
		const bool diagClear = access("/run/banana/3c-diag-clear", F_OK) == 0;
		const bool diagPlainFbo = access("/run/banana/3c-diag-plain-fbo", F_OK) == 0;
		const bool diagBoRender = access("/run/banana/3c-diag-bo-render", F_OK) == 0;
		const bool diagBoScanout = access("/run/banana/3c-diag-bo-scanout", F_OK) == 0;
		const bool diagBoScanoutFb = access("/run/banana/3c-diag-bo-scanout-fb", F_OK) == 0;
		const bool diagBoPrime = access("/run/banana/3c-diag-bo-prime", F_OK) == 0;
		const bool diagBoFbAfter = access("/run/banana/3c-diag-bo-fb-after", F_OK) == 0;
		const int diagCount = (diagImport ? 1 : 0) + (diagPbuffer ? 1 : 0) + (diagClear ? 1 : 0) +
		                      (diagPlainFbo ? 1 : 0) + (diagBoRender ? 1 : 0) + (diagBoScanout ? 1 : 0) +
		                      (diagBoScanoutFb ? 1 : 0) + (diagBoPrime ? 1 : 0) + (diagBoFbAfter ? 1 : 0);
		const bool freshWanted = access("/run/banana/3c-fresh-image", F_OK) == 0;
		DiagVariant diag = DiagVariant::None;
		bool diagRefused = false;
		if (diagCount > 1) { logf("more than one 3c diagnostic gate; diagnostics refused"); diagRefused = true; }
		else if (diagCount == 1 && freshWanted)
		{ logf("a 3c diagnostic gate cannot be combined with 3c-fresh-image; diagnostics refused"); diagRefused = true; }
		else if (diagImport) diag = DiagVariant::Import;
		else if (diagPbuffer) diag = DiagVariant::Pbuffer;
		else if (diagClear) diag = DiagVariant::Clear;
		else if (diagPlainFbo) diag = DiagVariant::PlainFbo;
		else if (diagBoRender) diag = DiagVariant::BoRender;
		else if (diagBoScanout) diag = DiagVariant::BoScanout;
		else if (diagBoScanoutFb) diag = DiagVariant::BoScanoutFb;
		else if (diagBoPrime) diag = DiagVariant::BoPrime;
		else if (diagBoFbAfter) diag = DiagVariant::BoFbAfter;

		// 3C is the default presenter. /data/banana/3c-direct is the durable escape hatch back to 3B's direct
		// path (create it and restart the menu). The EGL gate no longer selects the presenter; it and the trace
		// gate only turn on the durable diagnostic trace, which stays inert by default.
		const bool directWanted = access("/data/banana/3c-direct", F_OK) == 0;
		const bool eglGate = access("/run/banana/3c-egl", F_OK) == 0;
		bool eglWanted = !diagRefused && (diag != DiagVariant::None || !directWanted);
		bool traceWanted = eglWanted && (diag != DiagVariant::None || eglGate || access("/run/banana/3c-trace", F_OK) == 0);
		traceInit(traceWanted ? "/data/banana/logs/3c-trace" : nullptr);
		if (diag != DiagVariant::None)
		{
			if (eglGate) logf("3c-egl is ignored while a diagnostic variant is selected");
			tracef("start: diagnostic gate on");
		}
		else if (eglGate) tracef("start: EGL gate on");
		if (!eglWanted)
			logf(directWanted && !diagRefused ? "EGL rotation disabled by /data/banana/3c-direct; direct presenter"
			                                  : "EGL rotation disabled; direct presenter");
		else if (diag == DiagVariant::None && !eglGate) logf("EGL rotation enabled by default");
		{
			auto egl = std::make_unique<EglPresenter>(d, diag);
			std::string perr;
			if (eglWanted && egl->start(perr)) im->presenter = std::move(egl);
			else
			{
				if (eglWanted) logf("EGL rotation unavailable (%s)", perr.c_str());
				logf("direct (unrotated) present");
				auto direct = std::make_unique<DirectPresenter>(d);
				std::string derr;
				if (!direct->start(derr))
				{
					err = derr;
					wl_display_destroy(im->wl);
					delete im;
					return false;
				}
				logf("direct presenter active: the game frame is NOT rotated");
				im->presenter = std::move(direct);
			}
		}
		impl = im;
		// bananad uses this to write the 3D card file instead of the launcher/OSD paths while the compositor owns
		// the panel. Absent during a cage game or when the compositor is off.
		if (FILE* f = fopen("/run/banana/compositor", "w")) { fputs("banana-ui\n", f); fclose(f); }
		logf("wayland-0 at %s/%s (xdg-shell 1, linux-dmabuf 4 linear, wl_drm; primary %s; presenter %s)",
		     kWaylandDir, kSocket, im->devName, im->presenter->name());
		return true;
	}

	// Teardown contract (Phase 3C):
	//   1. no new frames; give an in-flight flip a bounded chance to complete, then put the display's own black
	//      buffer on the primary (and the overlay off) so no presenter-owned framebuffer is being scanned;
	//   2. drop the compositor's frame references (clients still own their wl_buffer resources);
	//   3. destroy the clients: client handlers, then resource callbacks, then bufferFree() -> presenter->forget()
	//      with the EGL context and the DRM fd still alive;
	//   4. stop the presenter: EGL images/textures/slots; framebuffers are only RmFB'd when step 1 said the plane
	//      was cleared (never while it can still scan one);
	//   5. destroy the Wayland display, unlink the socket, free the impl.
	// Every caller (runGame, SIGUSR1, ~Compositor) closes the Display immediately after this, so the DRM file close
	// releases any framebuffer ids left to it. No other code keeps a pool framebuffer id.
	void Compositor::stop()
	{
		if (!impl) return;
		Impl* im = impl;
		im->stopping = true;
		tracef("stop: begin");
		bool cleared = false;
		if (d.isOpen() && im->presenter)
		{
			for (int i = 0; i < 8 && d.flipPending(); i++)
			{
				pollfd p = { d.pollFd(), POLLIN, 0 };
				if (poll(&p, 1, 20) > 0) d.handleEvents();
			}
			if (!d.flipPending()) cleared = d.blank();
			tracef("stop: blank=%d", cleared);
			if (cleared) logf("stop: the primary no longer scans a presenter framebuffer");
			else logf("stop: the primary could not be cleared; the display close releases the scanout");
			im->presenter->quiesce(cleared);
		}
		if (im->pending) { im->bufferDrop(im->pending); im->pending = nullptr; }
		if (im->inflight) { im->bufferDrop(im->inflight); im->inflight = nullptr; }
		if (im->onScreen) { im->bufferDrop(im->onScreen); im->onScreen = nullptr; }
		im->pendingCb = im->inflightCb = im->pendCb = nullptr;
		wl_display_destroy_clients(im->wl);
		tracef("stop: clients destroyed");
		if (im->presenter) { im->presenter->stop(); im->presenter.reset(); }
		tracef("stop: presenter stopped");
		wl_display_destroy(im->wl);
		unlink((std::string(kWaylandDir) + "/" + kSocket).c_str());
		unlink("/run/banana/compositor");
		delete im;
		impl = nullptr;
	}

	int Compositor::wlFd() const { return impl && impl->loop ? wl_event_loop_get_fd(impl->loop) : -1; }

	void Compositor::dispatch()
	{
		if (!impl) return;
		wl_event_loop_dispatch(impl->loop, 0);
		wl_display_flush_clients(impl->wl);
		frame();
	}

	void Compositor::vblank()
	{
		if (impl) impl->vblank();
	}

	void Compositor::frame()
	{
		if (impl) impl->frame();
	}

	void Compositor::setOverlayFb(uint32_t fb)
	{
		if (!impl) return;
		impl->overlayFb = fb;
		// A client that is not committing (paused) must still show/hide the card: re-commit the frame that is on
		// screen with the new overlay. A commit already in flight picks the new overlay up on the next frame.
		if (impl->lastFb && impl->d->isOpen() && !impl->d->flipPending() && !impl->restoreMenu)
			impl->d->presentGame(impl->lastFb, impl->lastW, impl->lastH, fb);
	}

	bool Compositor::gameActive() const
	{
		return impl && (impl->pending || impl->inflight);
	}

	bool Compositor::clientOnScreen() const
	{
		return impl && impl->lastFb != 0;
	}

	bool Compositor::eglActive() const
	{
		return impl && impl->presenter && impl->presenter->rotated();
	}
}
