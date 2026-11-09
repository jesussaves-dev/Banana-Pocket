#pragma once
// The 90-degree clockwise turn as a coordinate map, shared by the EGL presenter and its host test. The reference is
// ui/src/turn.h's rot == 90 branch: dst[py][px] = src[H-1-px][py] for a target whose width is H and height is W.
// A 1240x1080 client image (W x H) therefore fills the 1080x1240 panel exactly: no scaling, no letterbox.
#include <cstdint>

namespace banana
{
	// target pixel (px, py) -> source pixel (x, y)
	inline void rotateSource(uint32_t px, uint32_t py, uint32_t W, uint32_t H, uint32_t& x, uint32_t& y)
	{
		x = py;
		y = H - 1 - px;
	}

	// the same at pixel centres, normalised for the quad's texture coordinates
	inline void rotateUv(uint32_t px, uint32_t py, uint32_t W, uint32_t H, float& u, float& v)
	{
		u = (py + 0.5f) / (float)W;
		v = (H - 0.5f - px) / (float)H;
	}
}
