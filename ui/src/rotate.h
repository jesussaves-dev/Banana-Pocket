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

	// A w x h source letterboxed (aspect kept, centred) into the W x H logical output, then turned 90 degrees onto
	// the H x W target: the viewport (x, y, vw, vh) in target pixels. The rect is centred, so it is the same in DRM
	// (top-left) and GL (bottom-left) coordinates. A W x H source fills the target exactly.
	inline void fitRotated(uint32_t w, uint32_t h, uint32_t W, uint32_t H, uint32_t& x, uint32_t& y, uint32_t& vw, uint32_t& vh)
	{
		uint32_t lw = W, lh = H;	// logical size of the fitted image
		if (w && h)
		{
			if ((uint64_t)w * H > (uint64_t)h * W) lh = (uint32_t)(((uint64_t)h * W + w / 2) / w);	// wider: bars top/bottom
			else lw = (uint32_t)(((uint64_t)w * H + h / 2) / h);						// taller: bars left/right
			if (!lw) lw = 1;
			if (!lh) lh = 1;
		}
		vw = lh;	// the turn swaps the axes
		vh = lw;
		x = (H - vw) / 2;
		y = (W - vh) / 2;
	}
}
