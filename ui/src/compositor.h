#pragma once
// The in-process compositor seam (Phase 3A). It owns the Display: atomic KMS with the black primary and the
// Banana UI on the overlay plane. Phase 3B adds the Wayland server and the single game client to this class;
// Phase 3C the EGL rotation into a linear scanout buffer. This is not a second executable and not a desktop
// compositor.
#include "kms.h"

namespace banana
{
	class Compositor
	{
	public:
		Display& display() { return d; }

	private:
		Display d;
		// Phase 3B: wl_display, one xdg toplevel, linux-dmabuf v4 feedback (linear only), seat, client lifetime.
		// Phase 3C: EGL context and the rotated linear scanout pool. Neither is implemented in 3A.
	};
}
