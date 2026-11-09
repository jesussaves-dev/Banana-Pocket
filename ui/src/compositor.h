#pragma once
// The in-process compositor seam. Phase 3A owns the Display: atomic KMS with the black primary and the Banana UI
// on the overlay plane. Phase 3B adds a minimal Wayland server: one kiosk client, linear DMA-BUF only, presented
// on the primary (the menu overlay is hidden while it runs). Phase 3C replaces the direct scanout with an EGL
// rotation into a linear buffer; the seam for that is Display::presentGame().
//
// This is not a second executable, not a general-purpose compositor, and not the game launch path: cage stays the
// default and this server is only reached by a client that connects to /run/banana/wayland/banana-0.
#include <cstdint>
#include <string>
#include "kms.h"

namespace banana
{
	class Compositor
	{
	public:
		Compositor() = default;
		~Compositor();
		Compositor(const Compositor&) = delete;
		Compositor& operator=(const Compositor&) = delete;

		Display& display() { return d; }

		// start/stop the Wayland server (idempotent). start() needs the atomic overlay path; false with a reason
		// when the panel is legacy, or another socket/global cannot be made.
		bool start(std::string& err);
		void stop();

		int wlFd() const;		// the Wayland event loop's fd for the main poll(), -1 when stopped
		void dispatch();		// drain Wayland events and flush replies
		void vblank();			// a page flip completed: release the scanned buffer, fire its callback
		void frame();			// present the latest committed client frame when no flip is in flight
		bool gameActive() const;	// a client frame is pending or on screen (the menu must not present)
		bool eglActive() const;		// the EGL rotate presenter is running (3C validation)

		struct Impl;			// the server state, defined in compositor.cpp

	private:
		Impl* impl = nullptr;
		Display d;
	};
}
