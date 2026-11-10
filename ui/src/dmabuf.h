#pragma once
// Phase 3B: the only DMA-BUF pair the Phase 2 matrix proved on this panel: XRGB8888/ARGB8888, one plane,
// linear. Kept here (header-only) so the host test can check the rules without DRM or Wayland.
// Compressed modifiers are never advertised or accepted.
#include <cstdint>
#include <drm_fourcc.h>

namespace banana
{
	inline bool dmabufSupported(uint32_t format, uint64_t modifier, int planes)
	{
		return planes == 1 && modifier == DRM_FORMAT_MOD_LINEAR &&
		       (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888);
	}
}
