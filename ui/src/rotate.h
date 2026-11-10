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

	// A w x h source letterboxed (aspect kept, centred) into a frame's hole (hx, hy, hw, hh) of the W x H logical
	// output, then turned 90 degrees onto the H x W target: the viewport (x, y, vw, vh) in target pixels. The target's
	// row 0 is the quad's bottom, so the numbers are the same in DRM and GL coordinates. A source of the hole's size
	// fills it exactly.
	inline void fitRotatedHole(uint32_t w, uint32_t h, uint32_t W, uint32_t H, uint32_t hx, uint32_t hy, uint32_t hw, uint32_t hh,
	                           uint32_t& x, uint32_t& y, uint32_t& vw, uint32_t& vh)
	{
		uint32_t lw = hw, lh = hh;	// logical size of the fitted image
		if (w && h)
		{
			if ((uint64_t)w * hh > (uint64_t)h * hw) lh = (uint32_t)(((uint64_t)h * hw + w / 2) / w);	// wider: bars top/bottom
			else lw = (uint32_t)(((uint64_t)w * hh + h / 2) / h);						// taller: bars left/right
			if (!lw) lw = 1;
			if (!lh) lh = 1;
		}
		vw = lh;	// the turn swaps the axes
		vh = lw;
		x = (H - hy - hh) + (hh - vw) / 2;	// logical row y is target column H-1-y
		y = hx + (hw - vh) / 2;
	}

	// the same with no frame: the whole output is the hole
	inline void fitRotated(uint32_t w, uint32_t h, uint32_t W, uint32_t H, uint32_t& x, uint32_t& y, uint32_t& vw, uint32_t& vh)
	{
		fitRotatedHole(w, h, W, H, 0, 0, W, H, x, y, vw, vh);
	}
}
