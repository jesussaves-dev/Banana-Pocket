// The system-wide shader: a CRT/LCD mask on the overlay plane and the panel's colour matrix. Pure, no DRM calls, so
// the patterns, the rotation and the composed base are unit-tested without a display (ui/tests/shader-mask-test.cpp)
// and `Display` only has to hand the result to KMS. See docs/PLAN: the system-wide shader plan.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <xf86drmMode.h>

namespace banana
{
	enum ShaderMask : uint8_t { SHADER_MASK_OFF, SHADER_MASK_CRT_240P, SHADER_MASK_APERTURE_GRILLE, SHADER_MASK_LCD_GRID, SHADER_MASK_COUNT };
	enum ColorProfile : uint8_t { COLOR_PROFILE_BYPASS, COLOR_PROFILE_TRINITRON_9300K, COLOR_PROFILE_PVM_6500K, COLOR_PROFILE_HANDHELD_LCD, COLOR_PROFILE_COUNT };

	// rowPeriod is the source pixel's height in screen pixels (one dark row per source row); originX/originY put the
	// pattern on the hole's first pixel, since plane rotation is rejected and the mask is generated pre-rotated.
	struct MaskGeom { int rowPeriod = 4, gridPeriod = 5, originX = 0, originY = 0; };

	// the mask's alpha at a logical (screen) pixel: RGB is always 0, so black stays #000000
	inline int maskMod(int v, int p) { if (p <= 0) return 0; int r = v % p; return r < 0 ? r + p : r; }
	inline uint8_t maskAlphaAt(ShaderMask m, const MaskGeom& g, int x, int y)
	{
		switch (m)
		{
		case SHADER_MASK_CRT_240P:
		{
			// one dark row per source row, the row above it slightly dimmed: 0x20 00 20 C0 (or 18 00 C0 at 3x)
			static const uint8_t p4[4] = { 0x20, 0x00, 0x20, 0xC0 };
			static const uint8_t p3[3] = { 0x18, 0x00, 0xC0 };
			int j = maskMod(y - g.originY, g.rowPeriod);
			if (g.rowPeriod == 4) return p4[j];
			if (g.rowPeriod == 3) return p3[j];
			// shortcut: only 3x and 4x are measured; every other period keeps one dark row and one dim row
			return j == g.rowPeriod - 1 ? 0xC0 : j == 0 ? 0x20 : 0x00;
		}
		case SHADER_MASK_APERTURE_GRILLE:
		{
			// three stripe columns, luma-coded so the average is neutral and black is untouched
			static const uint8_t r4[4][3] = { { 0x3D, 0x10, 0x6A }, { 0x30, 0x00, 0x60 }, { 0x3D, 0x10, 0x6A }, { 0xA5, 0x90, 0xBA } };
			static const uint8_t r3[3][3] = { { 0x3A, 0x0C, 0x67 }, { 0x30, 0x00, 0x60 }, { 0xA5, 0x90, 0xBA } };
			int j = maskMod(y - g.originY, g.rowPeriod), c = maskMod(x - g.originX, 3);
			return g.rowPeriod == 3 ? r3[j][c] : r4[j % 4][c];
		}
		case SHADER_MASK_LCD_GRID:
		{
			// a square cell of gridPeriod: its last column and row are the grid (0xA0), their crossing (0xC8) the corner
			int cx = maskMod(x - g.originX, g.gridPeriod), cy = maskMod(y - g.originY, g.gridPeriod);
			bool col = cx == g.gridPeriod - 1, row = cy == g.gridPeriod - 1;
			return col && row ? 0xC8 : (col || row) ? 0xA0 : 0x00;
		}
		default:
			return 0;
		}
	}

	// the panel-sized overlay buffer: logical scanlines become panel columns (plane rotation is rejected), W x H is
	// the composed screen, dst has the panel's own width and the buffer's real pitch in words
	inline void maskFill(uint32_t* dst, size_t pitchWords, int W, int H, int rot, ShaderMask m, const MaskGeom& g)
	{
		int pw = rot == 90 || rot == 270 ? H : W, ph = rot == 90 || rot == 270 ? W : H;
		for (int py = 0; py < ph; py++)
		{
			uint32_t* row = dst + (size_t)py * pitchWords;
			for (int px = 0; px < pw; px++)
			{
				int x, y;
				if (rot == 90) { x = py; y = H - 1 - px; }
				else if (rot == 270) { x = W - 1 - py; y = px; }
				else if (rot == 180) { x = W - 1 - px; y = H - 1 - py; }
				else { x = px; y = py; }
				row[px] = (uint32_t)maskAlphaAt(m, g, x, y) << 24;
			}
		}
	}

	// the composed base: the mask at `pct` with an optional opaque frame over it (the mask shows through the frame's
	// hole). Both inputs are premultiplied ARGB; the result has RGB 0 wherever nothing is over the mask.
	inline void maskCompose(uint32_t* base, const uint32_t* frameOrNull, const uint32_t* mask, size_t words, int pct)
	{
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		for (size_t i = 0; i < words; i++)
		{
			uint32_t ma = ((mask[i] >> 24) * (uint32_t)pct + 50) / 100;
			uint32_t a = ma, rgb = 0;
			if (frameOrNull)
			{
				uint32_t f = frameOrNull[i], fa = f >> 24;
				a = fa + (ma * (255 - fa) + 127) / 255;
				if (a > 255) a = 255;
				rgb = f & 0x00FFFFFFu;
			}
			base[i] = (a << 24) | rgb;
		}
	}

	// the mask's cell for the content: a system's integer scale, at the hole's own origin. Systems with a known
	// native width take it from their id; the rest from the hole width (256-wide at 4x -> 1024, 320-wide at 3x -> 960).
	inline MaskGeom maskGeomFor(const char* systemId, int holeX, int holeY, int holeW, bool integerScaled)
	{
		MaskGeom g;
		g.originX = holeX;
		g.originY = holeY;
		int scale = 0;
		const char* id = systemId ? systemId : "";
		if (!strcmp(id, "gb") || !strcmp(id, "gbc")) scale = 7;
		else if (!strcmp(id, "gba")) scale = 5;
		else if (!strcmp(id, "pico8")) scale = 8;
		else if (integerScaled && holeW > 0 && holeW % 320 == 0) scale = holeW / 320;
		else if (integerScaled && holeW > 0 && holeW % 256 == 0) scale = holeW / 256;
		g.rowPeriod = g.gridPeriod = scale > 0 ? scale : 4;	// unknown content: 4x, the pattern will not line up
		return g;
	}

	// the overlay plane's alpha for the slider's percent, in 16.16
	constexpr uint32_t maskPlaneAlpha(int pct) { return ((uint32_t)pct * 65535u + 50u) / 100u; }

	// the panel's GAMMA_LUT: the profile's curve (only PVM has one) with the night light's gains, so both share it.
	// Entry 0 stays 0: a black pixel must stay black.
	inline void colorLutFill(drm_color_lut* lut, unsigned n, ColorProfile c, int warmth)
	{
		if (!lut || n == 0) return;
		double kg = warmth >= 2 ? 0.85 : warmth == 1 ? 0.92 : 1.0;
		double kb = warmth >= 2 ? 0.65 : warmth == 1 ? 0.82 : 1.0;
		for (unsigned i = 0; i < n; i++)
		{
			double t = n > 1 ? (double)i / (n - 1) : 0.0;
			// gamma-encoded content (2.2) shown on a 2.4 display: the PVM profile's bend
			double v = 65535.0 * (c == COLOR_PROFILE_PVM_6500K ? std::pow(t, 2.4 / 2.2) : t);
			lut[i].red = (uint16_t)(v + 0.5);
			lut[i].green = (uint16_t)(v * kg + 0.5);
			lut[i].blue = (uint16_t)(v * kb + 0.5);
			lut[i].reserved = 0;
		}
		lut[0].red = lut[0].green = lut[0].blue = 0;
	}

	// 16.16, row-major, non-negative (CONVERT_S3_15 drops the sign bit), quantized to 2^-15 so the kernel's shift is
	// exact, and each row sums to at most 1.0. BYPASS is blob 0; the 9300K gains come from that white point, the
	// other two are starting choices to tune on the panel.
	inline constexpr uint64_t kColorCtm[COLOR_PROFILE_COUNT][9] = {
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 0x00000000C9B60000ull, 0x0000000004E00000ull, 0x0000000000F20000ull,
		  0x0000000002B00000ull, 0x00000000DD4E0000ull, 0x0000000001060000ull,
		  0x0000000003100000ull, 0x0000000006020000ull, 0x00000000F6EE0000ull },
		{ 0x00000000F53C0000ull, 0x0000000009040000ull, 0x0000000001C00000ull,
		  0x00000000047A0000ull, 0x00000000F36A0000ull, 0x0000000001B60000ull,
		  0x0000000004460000ull, 0x0000000008620000ull, 0x00000000E16C0000ull },
		{ 0x00000000BE080000ull, 0x000000002A600000ull, 0x00000000083A0000ull,
		  0x0000000014AA0000ull, 0x00000000C9DA0000ull, 0x0000000007E20000ull,
		  0x0000000011EA0000ull, 0x00000000232A0000ull, 0x00000000929A0000ull },
	};
}
