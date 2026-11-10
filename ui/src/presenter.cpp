#include "presenter.h"
#include "client_buffer.h"
#include "kms.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Phase 3B's presentation path, kept as the fallback when EGL rotation cannot start: the client's linear buffer is
// imported with AddFB2WithModifiers and scanned directly on the primary (the picture is sideways on this panel).
// It owns the imported framebuffer until forget(); the compositor's frame references keep the buffer alive while
// the plane can still be scanning it.
namespace banana
{
	bool DirectPresenter::start(std::string& err)
	{
		if (d.drmFd() < 0) { err = "the panel is not open"; return false; }
		return true;
	}

	bool DirectPresenter::render(ClientBuffer& b, uint32_t& fb, std::string& err)
	{
		if (!b.fb)
		{
			int fd = d.drmFd();
			if (fd < 0 || b.fd < 0) { err = "no DRM fd"; return false; }
			uint32_t handle = 0;
			if (drmPrimeFDToHandle(fd, b.fd, &handle)) { err = std::string("drmPrimeFDToHandle: ") + strerror(errno); return false; }
			uint32_t handles[4] = { handle }, pitches[4] = { b.stride }, offsets[4] = { b.off };
			uint64_t mods[4] = { b.modifier };
			int ret = drmModeAddFB2WithModifiers(fd, b.w, b.h, b.format, handles, pitches, offsets, mods, &b.fb, DRM_MODE_FB_MODIFIERS);
			drmCloseBufferHandle(fd, handle);
			if (ret) { b.fb = 0; err = std::string("AddFB2WithModifiers: ") + strerror(errno); return false; }
			close(b.fd);
			b.fd = -1;
		}
		fb = b.fb;
		return true;
	}

	void DirectPresenter::forget(ClientBuffer& b)
	{
		if (!b.fb) return;
		if (allowRmfb && d.drmFd() >= 0) drmModeRmFB(d.drmFd(), b.fb);
		b.fb = 0;
	}
}
