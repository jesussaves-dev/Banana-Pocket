// banana-ui: Settings in the SNES style -- the workshop. The groups are drawers down the left (the open one pulled
// out, gold), the selected group's rows on a sheet of paper on the right, the buttons along the bottom. Same rows,
// values, input and saving as the Classic screen (settings.cpp); only the drawing differs. It is laid out on the
// theme's 310-wide grid (u device px a unit, 4 on the handheld) in Banana's pixel font at size 12, over the theme's
// baked panels (tools/gen-street.py), so every edge lands on an art pixel.
#include "settings.h"
#include "i18n.h"
#include "raster.h"
#include <algorithm>
#include <cmath>

namespace banana
{
	using rnds::Color;

	void Settings::sFill(float x, float y, float w, float h, const Color& c)
	{
		be->fill(x * u, y * u, w * u, h * u, c);
	}

	// one line in Banana's pixel face at size 12 (crisp at whole units); its width in units
	float Settings::sText(const std::string& t, float x, float y, const Color& c, bool measureOnly)
	{
		if (t.empty()) return 0;
		if (!B) B = rnds::Font::load(bananaDir + "/fonts/BananaPixel.ttf");
		if (!B) return text(t, true, 12 * u / s, x * u / s, y * u / s, c, false, 0, measureOnly) * s / u;	// (no font: Pixelify)
		std::string key = "stxt:" + t;
		auto it = cache.find(key);
		if (it == cache.end())
		{
			rnds::TextStyle st;
			st.font = B; st.size = 12;
			rnds::TextLine l = rnds::layoutLine(rnds::utf8to32(t), st);
			rnds::TextBitmap tb = rnds::renderLine(l, st, u, st.font->ascent(12));
			Cached cc;
			if (!tb.bmp.empty()) cc.tex = be->upload(tb.bmp, true);
			cc.ox = tb.padX; cc.oy = tb.padY; cc.w = l.width;
			it = cache.emplace(key, cc).first;
		}
		const Cached& cc = it->second;
		if (!measureOnly && cc.tex)
			be->draw(cc.tex, std::floor(x * u) - cc.ox, std::floor(y * u) - cc.oy, (float)cc.tex->w, (float)cc.tex->h, c);
		return cc.w;
	}

	// t cut to width units, with "..." when it doesn't fit
	std::string Settings::sFit(const std::string& t, float width)
	{
		if (sText(t, 0, 0, sInk, true) <= width) return t;
		std::u32string w = rnds::utf8to32(t);
		while (!w.empty())
		{
			w.pop_back();
			std::string c;
			for (char32_t ch : w)
			{
				if (ch < 0x80) c += (char)ch;
				else if (ch < 0x800) { c += (char)(0xc0 | (ch >> 6)); c += (char)(0x80 | (ch & 0x3f)); }
				else { c += (char)(0xe0 | (ch >> 12)); c += (char)(0x80 | ((ch >> 6) & 0x3f)); c += (char)(0x80 | (ch & 0x3f)); }
			}
			if (sText(c + "...", 0, 0, sInk, true) <= width) return c + "...";
		}
		return "";
	}

	// one of the theme's baked panels, a texel a unit
	void Settings::sImg(const std::string& file, float x, float y)
	{
		if (snesArt.empty()) return;
		std::string key = "art:" + snesArt + "/" + file;
		auto it = cache.find(key);
		if (it == cache.end())
		{
			rnds::Bitmap b = rnds::decodeFile(snesArt + "/" + file);
			Cached c;
			if (!b.empty()) c.tex = be->upload(b, false);
			it = cache.emplace(key, c).first;
		}
		if (it->second.tex) be->draw(it->second.tex, x * u, y * u, it->second.tex->w * u, it->second.tex->h * u, Color(1, 1, 1, 1));
	}

	// a button and what it does: the key on a paper chip, the label beside it
	float Settings::sChipWidth(const std::string& key, const std::string& label)
	{
		return sText(key, 0, 0, sInk, true) + 4 + 3 + sText(label, 0, 0, sPaper, true);
	}

	void Settings::sChip(const std::string& key, const std::string& label, float x, float y)
	{
		float kw = sText(key, 0, 0, sInk, true) + 4;
		sFill(x, y, kw, 13, sInk);
		sFill(x + 1, y + 1, kw - 2, 11, sPaper);
		sText(key, x + 2, y, sInk);
		sText(label, x + kw + 4, y + 1, sInk);
		sText(label, x + kw + 3, y, sPaper);
	}

	void Settings::renderWorkshop()
	{
		const int ng = std::min<int>((int)groups.size(), 8);
		const int g = std::max(0, std::min(inGroups ? gsel : groupOf(sel), ng - 1));
		const int gb = groupStart(g), ge = groupEnd(g);
		const Color ink = sInk, paper = sPaper;

		be->beginScreen();
		sFill(0, 0, 310, 270, sDim);
		sImg("workshop.png", 0, 0);
		// the header: where this is, and what is open
		sText(tr("Workshop"), 10, 6, ink);
		sText(tr("Workshop"), 9, 5, sGold);
		std::string right = inGroups ? tr("Settings") : std::to_string(sel - gb + 1) + "/" + std::to_string(ge - gb);
		float rw = sText(right, 0, 0, paper, true);
		sText(right, 301 - rw, 5, paper);

		// the drawers: one a group, the open (or chosen) one pulled out
		for (int i = 0; i < ng; i++)
		{
			const bool on = i == g;
			const float y = 26 + i * 28;
			sImg(on ? "drawer-on.png" : "drawer.png", 0, y);
			std::string label = sFit(tr(groups[i]), on ? 100 : 90);
			sText(label, 7, y + 6, on ? ink : ink);
			if (on && inGroups)
			{
				// chosen, not yet open: how many settings are in it, on a little tag
				std::string n = std::to_string(ge - gb);
				float nw = sText(n, 0, 0, ink, true) + 4;
				sFill(118 - nw, y + 3, nw, 12, ink);
				sFill(119 - nw, y + 4, nw - 2, 10, paper);
				sText(n, 120 - nw, y + 3, ink);
			}
		}

		// the sheet: the group's rows, the selected one gold when the drawer is open
		const float sx = 132, sy = 24, sw = 170;
		sImg("sheet.png", sx, sy);
		sText(sFit(tr(groups[g]), sw - 16), sx + 8, sy + 5, ink);
		const int vis = 10;
		int first = gb;
		if (!inGroups) first = std::max(gb, std::min(sel - vis / 2, ge - vis));
		first = std::max(gb, first);
		for (int i = first; i < std::min(ge, first + vis); i++)
		{
			const Item& it = items[i];
			const float y = sy + 24 + (i - first) * 16;
			const bool on = !inGroups && i == sel;
			if (on)
			{
				sFill(sx + 3, y - 2, sw - 6, 15, sGold);
				sFill(sx + 3, y - 2, sw - 6, 1, Color(1, 1, 1, 0.55f));
			}
			std::string v = rowValue(i);
			if (!v.empty()) v = tr(v);
			const bool slider = it.kind == Item::BRIGHTNESS || it.kind == Item::VOLUME || it.kind == Item::PERCENT;
			const bool arrows = it.kind == Item::CHOICE && on;
			float valueW = slider ? 50 : v.empty() ? 0 : std::min(96.0f, sText(v, 0, 0, ink, true) + (arrows ? 12 : 0));
			std::string label = sFit(tr(it.label), sw - 16 - valueW - (valueW > 0 ? 6 : 0));
			sText(label, sx + 8, y, ink);
			const float vr = sx + sw - 8;	// the value's right edge
			if (slider)
			{
				int pct = atoi(it.values[it.at].c_str());
				if (it.kind == Item::BRIGHTNESS) pct /= 10;
				pct = std::min(100, std::max(0, pct));
				for (int k = 0; k < 10; k++)
					sFill(vr - 50 + k * 5, y + 1, 4, 9, k < (pct + 5) / 10 ? ink : (on ? Color(ink.r, ink.g, ink.b, 0.25f) : sMuted));
				continue;
			}
			if (v.empty()) continue;
			std::string vs = sFit(v, 96 - (arrows ? 12 : 0));
			float vw = sText(vs, 0, 0, ink, true);
			const Color vc = on ? ink : sDim;
			if (arrows)
			{
				// < value >: stepped triangles on the grid
				for (int k = 0; k < 3; k++) { sFill(vr - 3 + k, y + 2 + k, 1, 7 - 2 * k, ink); sFill(vr - vw - 9 - k, y + 2 + k, 1, 7 - 2 * k, ink); }
				sText(vs, vr - 6 - vw, y, vc);
			}
			else sText(vs, vr - vw, y, vc);
		}

		// under the rows: what the chosen group holds, or what the selected setting does
		std::string about;
		if (inGroups)
		{
			for (int i = gb; i < ge; i++) about += (about.empty() ? "" : ", ") + tr(items[i].label);
		}
		else about = rowHelp();
		{
			rnds::TextStyle st;
			if (!B) B = rnds::Font::load(bananaDir + "/fonts/BananaPixel.ttf");
			st.font = B; st.size = 12;
			if (B)
			{
				std::vector<rnds::TextLine> lines = rnds::layoutWrap(rnds::utf8to32(about), st, (int)(sw - 16), 2);
				for (size_t k = 0; k < lines.size(); k++)
				{
					std::string u8;
					for (char32_t ch : lines[k].text)
					{
						if (ch < 0x80) u8 += (char)ch;
						else if (ch < 0x800) { u8 += (char)(0xc0 | (ch >> 6)); u8 += (char)(0x80 | (ch & 0x3f)); }
						else { u8 += (char)(0xe0 | (ch >> 12)); u8 += (char)(0x80 | ((ch >> 6) & 0x3f)); u8 += (char)(0x80 | (ch & 0x3f)); }
					}
					sText(u8, sx + 8, sy + 188 + k * 12, sDim);
				}
			}
		}

		// the buttons
		const Item& cur = items[sel];
		std::string act = inGroups ? tr("Open") : cur.kind == Item::ACTION ? (armed == sel ? tr("Confirm") : tr(cur.label)) :
			cur.kind == Item::INFO ? "" : cur.key == "scrape" ? (scraping() ? "" : tr("Start")) : cur.kind == Item::PAGE ? tr("Open") : tr("Change");
		sChip("B", inGroups ? tr("Close") : tr("Back"), 8, 254);
		if (!act.empty()) sChip("A", act, 302 - sChipWidth("A", act), 254);
		be->endScreen();
	}
}
