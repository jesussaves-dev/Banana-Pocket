#include "frame.h"
#include <algorithm>
#include <cstring>
#include "../third_party/stb_image.h"

namespace banana
{
	namespace
	{
		constexpr int kBorder = 32;	// the Banana frame's least width on every side
		constexpr int kMinHole = 64;	// less than this is stray transparent pixels, not a hole

		struct Aspect { const char* id; int w, h; };
		// systems whose picture is not 4:3; w == 0 takes the whole hole (a port sizes itself to its window)
		const Aspect ASPECTS[] = {
			{ "gb", 10, 9 }, { "gbc", 10, 9 }, { "gamegear", 10, 9 }, { "gba", 3, 2 }, { "ngp", 20, 19 },
			{ "wonderswan", 14, 9 }, { "atarilynx", 80, 51 }, { "ports", 0, 0 },
		};

		uint32_t opaque(uint32_t rgb) { return 0xff000000u | (rgb & 0xffffffu); }
	}

	const char* frameKindWord(FrameKind k)
	{
		return k == FrameKind::Art ? "art" : k == FrameKind::Banana ? "banana" : "none";
	}

	FrameKind resolveFrame(bool master, const std::string& game, const std::string& system, bool hasArt)
	{
		if (!master) return FrameKind::None;
		std::string v = game.empty() || game == "auto" ? system : game;
		if (v.empty() || v == "auto") return hasArt ? FrameKind::Art : FrameKind::None;
		if (v == "art") return hasArt ? FrameKind::Art : FrameKind::Banana;
		if (v == "banana") return FrameKind::Banana;
		return FrameKind::None;
	}

	FrameHole frameBananaHole(const std::string& systemId, int W, int H)
	{
		int aw = 4, ah = 3;
		for (const Aspect& a : ASPECTS) if (systemId == a.id) { aw = a.w; ah = a.h; }
		int maxW = W - 2 * kBorder, maxH = H - 2 * kBorder;
		FrameHole r;
		if (!aw) { r.w = maxW; r.h = maxH; }
		else
		{
			r.w = maxW;
			r.h = (int)(((int64_t)maxW * ah + aw / 2) / aw);
			if (r.h > maxH)
			{
				r.h = maxH;
				r.w = (int)(((int64_t)maxH * aw + ah / 2) / ah);
			}
		}
		r.w &= ~1;	// even, so the hole centres on whole pixels
		r.h &= ~1;
		r.x = (W - r.w) / 2;
		r.y = (H - r.h) / 2;
		return r;
	}

	FrameHole frameHoleOf(const uint32_t* argb, int W, int H)
	{
		int x0 = W, y0 = H, x1 = -1, y1 = -1;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				if (!(argb[(size_t)y * W + x] >> 24))
				{
					x0 = std::min(x0, x); x1 = std::max(x1, x);
					y0 = std::min(y0, y); y1 = std::max(y1, y);
				}
		FrameHole r;
		if (x1 - x0 + 1 < kMinHole || y1 - y0 + 1 < kMinHole) return r;
		r.x = x0; r.y = y0; r.w = x1 - x0 + 1; r.h = y1 - y0 + 1;
		return r;
	}

	bool frameLoadArt(const std::string& path, int W, int H, std::vector<uint32_t>& out, FrameHole& hole, std::string& err)
	{
		int w = 0, h = 0, n = 0;
		if (!stbi_info(path.c_str(), &w, &h, &n)) { err = path + ": not a picture"; return false; }
		if (w != W || h != H) { err = path + ": " + std::to_string(w) + "x" + std::to_string(h) + ", not the panel's size"; return false; }
		unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
		if (!px) { err = path + ": can't decode it"; return false; }
		out.resize((size_t)W * H);
		for (size_t i = 0; i < out.size(); i++)
		{
			const unsigned char* s = px + i * 4;
			uint32_t a = s[3];
			auto pm = [&](uint32_t c) { return (c * a + 127) / 255; };
			out[i] = a << 24 | pm(s[0]) << 16 | pm(s[1]) << 8 | pm(s[2]);
		}
		stbi_image_free(px);
		hole = frameHoleOf(out.data(), W, H);
		if (!hole.w) { err = path + ": no transparent hole"; return false; }
		return true;
	}

	void frameDrawBanana(std::vector<uint32_t>& out, int W, int H, const FrameHole& hole, const FrameColors& c)
	{
		out.assign((size_t)W * H, opaque(c.backdrop));
		auto rect = [&](int x0, int y0, int x1, int y1, uint32_t v) {
			x0 = std::max(x0, 0); y0 = std::max(y0, 0);
			x1 = std::min(x1, W); y1 = std::min(y1, H);
			for (int y = y0; y < y1; y++) std::fill(out.begin() + (size_t)y * W + x0, out.begin() + (size_t)y * W + std::max(x0, x1), v);
		};
		// the menu's dot grid: 4 px dots every 16 px, on the same chunky pixel as everything below
		for (int y = 6; y < H; y += 16)
			for (int x = 6; x < W; x += 16) rect(x, y, x + 4, y + 4, opaque(c.dots));
		// a band around the hole, lit from the top left, its corners stepped off by one chunky pixel
		constexpr int P = 4, band = 28;
		const int x0 = hole.x - band, y0 = hole.y - band, x1 = hole.x + hole.w + band, y1 = hole.y + hole.h + band;
		rect(x0 + P, y0, x1 - P, y1, opaque(c.body));
		rect(x0, y0 + P, x1, y1 - P, opaque(c.body));
		rect(x0 + P, y0, x1 - P, y0 + P, opaque(c.band));
		rect(x0, y0 + P, x0 + P, y1 - P, opaque(c.band));
		rect(x0 + P, y1 - P, x1 - P, y1, opaque(c.low));
		rect(x1 - P, y0 + P, x1, y1 - P, opaque(c.low));
		// an accent line, then a dark ring that meets the picture
		rect(hole.x - 12, hole.y - 12, hole.x + hole.w + 12, hole.y + hole.h + 12, opaque(c.accent));
		rect(hole.x - 8, hole.y - 8, hole.x + hole.w + 8, hole.y + hole.h + 8, opaque(c.ring));
		rect(hole.x, hole.y, hole.x + hole.w, hole.y + hole.h, 0);
	}

	void frameDim(uint32_t* argb, size_t n, int percent)
	{
		if (percent >= 100) return;
		uint32_t k = (uint32_t)std::max(percent, 0);
		for (size_t i = 0; i < n; i++)
		{
			uint32_t v = argb[i];
			uint32_t r = ((v >> 16 & 0xff) * k + 50) / 100, g = ((v >> 8 & 0xff) * k + 50) / 100, b = ((v & 0xff) * k + 50) / 100;
			argb[i] = (v & 0xff000000u) | r << 16 | g << 8 | b;
		}
	}
}
