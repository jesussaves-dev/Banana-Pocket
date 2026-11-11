#include "frontend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

namespace rnds
{
	std::string brandNote;
	Accent accent = { 0xf0b429, 0xffe68a, 0x8a5a00, 0xffcf5c, 0xc98f12 };
	static const struct { const char* name; Accent a; } ACCENTS[] = {
		{ "yellow", { 0xf0b429, 0xffe68a, 0x8a5a00, 0xffcf5c, 0xc98f12 } },
		{ "blue",   { 0x4a9fe0, 0xb7e1ff, 0x0f3f66, 0x7cc0f2, 0x2f7fc0 } },
		{ "red",    { 0xe8574d, 0xffb8b0, 0x7a1a14, 0xf58a80, 0xc23b32 } },
		{ "green",  { 0x56bf6b, 0xbff0c8, 0x1d5a2b, 0x86d896, 0x3d9a52 } },
		{ "purple", { 0xa07ae6, 0xddd0ff, 0x43266e, 0xbfa4f2, 0x7f58c4 } },
		{ "pink",   { 0xec6fa8, 0xffc6e0, 0x7a1f4a, 0xf59cc4, 0xc9508a } },
	};
	std::map<std::string, uint32_t> themeColors;
	ThemeFiles themeFiles;
	void setAccent(const Accent& a) { accent = a; }
	bool themeColor(const char* name, Color& c)
	{
		auto it = themeColors.find(name);
		if (it == themeColors.end()) return false;
		uint32_t v = it->second;
		c = Color(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, ((v >> 24) & 0xff) / 255.f);
		return true;
	}
	bool setAccent(const std::string& name)
	{
		for (auto& e : ACCENTS) if (name == e.name) { accent = e.a; return true; }
		accent = ACCENTS[0].a;
		return false;
	}
	static void rgb2hsv(float r, float g, float b, float& h, float& s, float& v)
	{
		float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
		v = mx; s = mx > 0 ? d / mx : 0; h = 0;
		if (d > 0) h = mx == r ? 60 * std::fmod((g - b) / d + 6, 6.0f) : mx == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
	}
	static void hsv2rgb(float h, float s, float v, float& r, float& g, float& b)
	{
		h = std::fmod(h + 360, 360.0f);
		float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = v - c;
		int k = (int)(h / 60) % 6;
		float rr[6] = { c, x, 0, 0, x, c }, gg[6] = { x, c, c, x, 0, 0 }, bb[6] = { 0, 0, x, c, c, x };
		r = rr[k] + m; g = gg[k] + m; b = bb[k] + m;
	}
	// a yellow (hue 25-70, saturated) moved by the yellow's hue, saturation and value to the accent's: the same shading
	static bool tintRgb(uint8_t& r, uint8_t& g, uint8_t& b)
	{
		if (accent.base == 0xf0b429) return false;
		float h, s, v; rgb2hsv(r / 255.f, g / 255.f, b / 255.f, h, s, v);
		if (h < 25 || h > 70 || s < 0.3f || v < 0.12f) return false;
		float ah, as, av, yh, ys, yv;
		rgb2hsv(((accent.base >> 16) & 255) / 255.f, ((accent.base >> 8) & 255) / 255.f, (accent.base & 255) / 255.f, ah, as, av);
		rgb2hsv(0xf0 / 255.f, 0xb4 / 255.f, 0x29 / 255.f, yh, ys, yv);
		float nr, ng, nb;
		hsv2rgb(h - yh + ah, std::min(1.0f, s * as / ys), std::min(1.0f, v * av / yv), nr, ng, nb);
		r = (uint8_t)std::lround(nr * 255); g = (uint8_t)std::lround(ng * 255); b = (uint8_t)std::lround(nb * 255);
		return true;
	}
	void tintAccent(Bitmap& bm)
	{
		if (bm.alpha || accent.base == 0xf0b429) return;
		for (size_t i = 0; i + 3 < bm.px.size(); i += 4) tintRgb(bm.px[i], bm.px[i + 1], bm.px[i + 2]);
	}
	uint32_t tintAccent(uint32_t c)
	{
		uint8_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
		if (!tintRgb(r, g, b)) return c;
		return (c & 0xff000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
	}

	// ---- timing ---------------------------------------------------------------------------------------------------------
}
