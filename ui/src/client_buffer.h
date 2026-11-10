#pragma once
// One client wl_buffer: the import parameters plus the per-presenter cache. refs counts the owners (the wl_buffer
// resource itself, and each frame slot that holds it); the object is freed only when the last owner drops it.
// The presenter owns the GPU/DRM objects it puts in here and frees them in forget() while its context is alive.
#include <cstdint>

struct wl_resource;

namespace banana
{
	struct ClientBuffer
	{
		void* im = nullptr;			// Compositor::Impl, for refcount helpers
		wl_resource* res = nullptr;		// the wl_buffer resource
		int fd = -1;				// the source dma-buf (closed once a presenter has imported it)
		uint32_t off = 0, stride = 0, format = 0, w = 0, h = 0;
		uint64_t modifier = 0;
		uint32_t fb = 0;			// DirectPresenter: the imported DRM framebuffer
		void* eglImage = nullptr;		// EglPresenter: the EGLImage of this source
		unsigned tex = 0;			// EglPresenter: the GL texture made from it
		uint64_t serial = 0;			// trace identity of this wl_buffer
		int refs = 0;
	};
}
