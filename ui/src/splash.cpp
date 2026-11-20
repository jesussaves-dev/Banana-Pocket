#include "splash.h"
#include "frontend.h"
#include <algorithm>
#include <cmath>

namespace banana
{
	// The GBA BIOS intro's own clock, at 60 fps: the letters seat at frame 108, the shine runs to frame 210, then the
	// fade. SEAT_AT and INTRO_END are those frames in ms.
	static constexpr double MS_PER_FRAME = 1000.0 / 60.0;
	static constexpr double SEAT_AT = 1800, SHINE_AT = 1800, SHINE = 1700;
	static constexpr double FADE_AT = 3450, FADE = 250;
	static constexpr int GW = 240, GH = 160, WORD_Y = 32, ICON_Y = 100;
	static constexpr int STAGGER_FRAMES = 5, FLIGHT_FRAMES = 75;
	static constexpr int FIRST_FRAME = 8;
	static_assert(Splash::HOLD_AT < FIRST_FRAME * MS_PER_FRAME, "the audio hold must release before the first letter moves");

	static inline uint32_t mul255(uint32_t p, uint32_t k)
	{
		uint32_t rb = (p & 0x00ff00ffu) * k + 0x00800080u;
		rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
		uint32_t ag = ((p >> 8) & 0x00ff00ffu) * k + 0x00800080u;
		ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
		return rb | ag;
	}

	static inline uint32_t over(uint32_t s, uint32_t d)
	{
		uint32_t a = s >> 24;
		if (a == 255) return s;
		if (a == 0) return d;
		return s + mul255(d, 255 - a);
	}

	// an opaque lerp, channel by channel
	static inline uint32_t mix(uint32_t a, uint32_t b, double f)
	{
		f = std::clamp(f, 0.0, 1.0);
		int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
		int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
		return 0xff000000u | (uint32_t)std::lround(ar + (br - ar) * f) << 16
		                   | (uint32_t)std::lround(ag + (bg - ag) * f) << 8
		                   | (uint32_t)std::lround(ab + (bb - ab) * f);
	}

	static bool loadRgba(const std::string& path, std::vector<uint32_t>& px, int& w, int& h)
	{
		rnds::Bitmap b = rnds::decodeFile(path);
		if (b.empty()) return false;
		w = b.w; h = b.h;
		px.resize((size_t)w * h);
		for (size_t i = 0; i < px.size(); i++)
		{
			const uint8_t* s = &b.px[i * 4];
			uint32_t a = s[3];
			auto m = [a](uint32_t v) { uint32_t x = v * a + 128; return (x + (x >> 8)) >> 8; };
			px[i] = rnds::tintAccent((a << 24) | (m(s[0]) << 16) | (m(s[1]) << 8) | m(s[2]));	// the accent's banana
		}
		return true;
	}

	// a word's mask (white on black), with each letter's column range found from the empty columns between them
	static bool loadMask(const std::string& path, int& w, int& h, std::vector<uint8_t>& a,
	                     std::vector<int>& letters, std::vector<int>& ends)
	{
		rnds::Bitmap b = rnds::decodeFile(path);
		if (b.empty()) return false;
		w = b.w; h = b.h;
		a.resize((size_t)w * h);
		for (size_t i = 0; i < a.size(); i++) a[i] = b.px[i * 4] > 127 ? 255 : 0;
		letters.clear(); ends.clear();
		int start = -1;
		for (int x = 0; x < w; x++)
		{
			bool any = false;
			for (int y = 0; y < h; y++) any |= a[(size_t)y * w + x] != 0;
			if (any && start < 0) start = x;
			else if (!any && start >= 0) { letters.push_back(start); ends.push_back(x); start = -1; }
		}
		if (start >= 0) { letters.push_back(start); ends.push_back(w); }
		return true;
	}

	bool Splash::load(const std::string& dir, const std::string& banana, const std::string& variant, int w, int h)
	{
		// a 240x160 GBA scene, integer-scaled and centred: 5x (1200x800) on the RPC's 1240x1080, 2x on the host test's
		W = w; H = h;
		g = std::max(1, std::min(w / GW, h / GH));
		ox = (w - GW * g) / 2;
		oy = (h - GH * g) / 2;
		if (!loadRgba(banana, bunch.px, bunch.w, bunch.h)) return false;
		if (!loadMask(dir + "/word.png", word.w, word.h, word.a, word.letters, word.ends)) return false;
		if (word.w < 1 || word.h < 1 || word.letters.empty()) return false;
		// the wordmark is 32 virtual px tall, like the GBA's glyphs
		KW = std::max(1, (int)std::lround(g * 32.0 / word.h));
		(void)variant;
		return true;
	}

	int Splash::VX(float v) const { return ox + (int)std::lround(v * g); }
	int Splash::VY(float v) const { return oy + (int)std::lround(v * g); }

	void Splash::fill(uint32_t* d, int x, int y, int w, int h, uint32_t c) const
	{
		int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(W, x + w), y1 = std::min(H, y + h);
		for (int yy = y0; yy < y1; yy++)
			for (int xx = x0; xx < x1; xx++) d[(size_t)yy * W + xx] = over(c, d[(size_t)yy * W + xx]);
	}

	// the bunch, in the Nintendo wordmark's place, at k device px per sprite px
	void Splash::drawBunch(uint32_t* d, int x, int y, int k, int alpha) const
	{
		const int Wd = bunch.w * k, Hd = bunch.h * k;
		if (Wd <= 0 || Hd <= 0) return;
		for (int dy = 0; dy < Hd; dy++)
		{
			const int yy = y + dy;
			if (yy < 0 || yy >= H) continue;
			const uint32_t* srow = &bunch.px[(size_t)(dy / k) * bunch.w];
			uint32_t* drow = d + (size_t)yy * W;
			for (int dx = 0; dx < Wd; dx++)
			{
				uint32_t p = srow[dx / k];
				if (!(p >> 24)) continue;
				if (alpha < 255) p = mul255(p, (uint32_t)alpha);
				const int xx = x + dx;
				if (xx >= 0 && xx < W) drow[xx] = over(p, drow[xx]);
			}
		}
	}

	// a rainbow colour for h in 0..1 (hue wheel, full saturation and value): no tables, no allocation
	static uint32_t rainbow(double h)
	{
		h = h - std::floor(h);
		double x = h * 6, f = x - std::floor(x);
		double r = 0, g = 0, b = 0;
		switch ((int)x)
		{
		case 0: r = 1; g = f; break;
		case 1: r = 1 - f; g = 1; break;
		case 2: g = 1; b = f; break;
		case 3: g = 1 - f; b = 1; break;
		case 4: r = f; b = 1; break;
		default: r = 1; b = 1 - f; break;
		}
		return 0xff000000u | ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
	}

	// one letter, scaled about its centre: k device px per mask px, sheared to italic, in its flight colour mixed to
	// the banana tones by mixf, with the shine band over it after it seats
	void Splash::drawLetter(uint32_t* d, int c0, int c1, double cx, double cy, double k, double t, double mixf, int i) const
	{
		const int kw = std::max(1, (int)std::lround(k));
		const int lw = (c1 - c0) * kw, lh = word.h * kw;
		const int x0 = (int)std::lround(cx - lw * 0.5), y0 = (int)std::lround(cy - lh * 0.5);
		const int lightRow = (word.h * 11 + 19) / 20;
		const uint32_t banana[2] = { rnds::tintAccent(0xffffde52u), rnds::tintAccent(0xffeea018u) };
		const uint32_t flight = rainbow(i * 0.17 + t * 0.0006);
		const int wordX = (W - word.w * KW) / 2;
		const int bandW = std::max(1, word.w * KW / 4);
		const bool shining = t >= SHINE_AT && t < SHINE_AT + SHINE;
		const int bandX = shining ? (int)std::lround((t - SHINE_AT) / SHINE * (word.w * KW + 2 * bandW)) - bandW : -1000000;
		for (int my = 0; my < word.h; my++)
		{
			const uint32_t base = mix(flight, banana[my < lightRow ? 0 : 1], mixf);
			const int shear = (word.h - 1 - my) * kw / 4;
			for (int mx = c0; mx < c1; mx++)
			{
				if (!word.a[(size_t)my * word.w + mx]) continue;
				const int x = x0 + (mx - c0) * kw + shear;
				const int rel = x - wordX;
				const uint32_t c = rel >= bandX && rel < bandX + bandW ? rnds::tintAccent(0xfffff0b4u) : base;
				fill(d, x, y0 + my * kw, kw, kw, c);
			}
		}
	}

	void Splash::draw(uint32_t* scr, double t, bool ready)
	{
		// The white GBA screen; the bunch fades in under the wordmark; the six letters fly in from the bottom-right,
		// tiny at first, and ease onto their seats at SEAT_AT, their colour chase settling to the banana tones. Then
		// the shine sweeps the wordmark, and the last 250 ms fade to white so the iris closes on white.
		std::fill(scr, scr + (size_t)W * H, 0xffffffffu);

		const int alpha = (int)std::lround(255 * std::clamp(t / 300.0, 0.0, 1.0));
		drawBunch(scr, (W - bunch.w * g) / 2, VY(ICON_Y), g, alpha);

		const int n = (int)word.letters.size();
		const int wordX = (W - word.w * KW) / 2;
		const double cy = VY(WORD_Y) + word.h * KW * 0.5;
		const double flight = FLIGHT_FRAMES * MS_PER_FRAME;
		for (int i = 0; i < n; i++)
		{
			const int c0 = word.letters[i], c1 = word.ends[i];
			const double s0 = (FIRST_FRAME + STAGGER_FRAMES * (n - 1 - i)) * MS_PER_FRAME;
			const double p = std::clamp((t - s0) / flight, 0.0, 1.0);
			const double e = 1.0 - std::pow(1.0 - p, 3.0);
			const double k = KW * (0.04 + 0.96 * e);
			const double cx = wordX + (c0 + c1) * 0.5 * KW;
			const double dx = (n - 1 - i) * 70.0 * g, dy = 130.0 * g;
			const double mixf = std::clamp((e - 0.7) / 0.3, 0.0, 1.0);
			drawLetter(scr, c0, c1, cx + dx * (1.0 - e), cy + dy * (1.0 - e), k, t, mixf, i);
		}

		if (!cued && t >= SEAT_AT) { cued = true; if (onCue) onCue("insert"); }

		if (ready && t >= FADE_AT)
		{
			// a 256-entry table, so the fade's whole-canvas pass is integer lookups, not three lerps a pixel
			const int k = (int)std::lround(255 * std::clamp((t - FADE_AT) / FADE, 0.0, 1.0));
			uint8_t lut[256];
			for (int v = 0; v < 256; v++) lut[v] = (uint8_t)((v * (255 - k) + 255 * k + 127) / 255);
			if (k > 0)
				for (size_t i = 0; i < (size_t)W * H; i++)
				{
					const uint32_t c = scr[i];
					scr[i] = 0xff000000u | (uint32_t)lut[(c >> 16) & 255] << 16
					                   | (uint32_t)lut[(c >> 8) & 255] << 8
					                   | (uint32_t)lut[c & 255];
				}
		}
	}

	int Splash::nextFrame(double t, bool ready) const
	{
		if (t < INTRO_END) return 16;
		// holding on the seated wordmark until the library is ready
		return ready ? 0 : 100;
	}
}
