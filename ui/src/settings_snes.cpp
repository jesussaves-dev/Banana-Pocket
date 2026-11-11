// banana-ui: Settings in the SNES style -- the workshop, the phone book (the keyboard) and the ticket (the pause menu).
// The workshop: the groups are drawers down the left (the open one pulled
// out, gold), the selected group's rows on a sheet of paper on the right, the buttons along the bottom. Same rows,
// values, input and saving as the Classic screen (settings.cpp); only the drawing differs. It is laid out on the
// theme's 310-wide grid (u device px a unit, 4 on the handheld) in Banana's pixel font at size 12, over the theme's
// baked panels (tools/gen-street.py), so every edge lands on an art pixel.
#include "settings.h"
#include "i18n.h"
#include "raster.h"
#include <algorithm>
#include <cmath>
#include <sys/stat.h>

namespace banana
{
	using rnds::Color;

	static std::string utf8(const std::u32string& w)
	{
		std::string c;
		for (char32_t ch : w)
		{
			if (ch < 0x80) c += (char)ch;
			else if (ch < 0x800) { c += (char)(0xc0 | (ch >> 6)); c += (char)(0x80 | (ch & 0x3f)); }
			else { c += (char)(0xe0 | (ch >> 12)); c += (char)(0x80 | ((ch >> 6) & 0x3f)); c += (char)(0x80 | (ch & 0x3f)); }
		}
		return c;
	}

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
			std::string c = utf8(w);
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

	// t wrapped to width units, at most lines lines of 12
	void Settings::sWrap(const std::string& t, float x, float y, float width, int lines, const Color& c)
	{
		if (!B) B = rnds::Font::load(bananaDir + "/fonts/BananaPixel.ttf");
		if (!B || t.empty()) return;
		rnds::TextStyle st;
		st.font = B; st.size = 12;
		std::vector<rnds::TextLine> ls = rnds::layoutWrap(rnds::utf8to32(t), st, (int)width, lines);
		for (size_t k = 0; k < ls.size(); k++) sText(utf8(ls[k].text), x, y + k * 12, c);
	}

	// < value >: stepped triangles on the grid around a value of valueW ending 6 units short of right
	void Settings::sArrows(float right, float y, float valueW, const Color& c)
	{
		for (int k = 0; k < 3; k++)
		{
			sFill(right - 3 + k, y + 2 + k, 1, 7 - 2 * k, c);
			sFill(right - valueW - 10 - k, y + 2 + k, 1, 7 - 2 * k, c);
		}
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
			float valueW = slider ? 50 : v.empty() ? 0 : std::min(96.0f, sText(v, 0, 0, ink, true) + (arrows ? 13 : 0));
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
			std::string vs = sFit(v, 96 - (arrows ? 13 : 0));
			float vw = sText(vs, 0, 0, ink, true);
			const Color vc = on ? ink : sDim;
			if (arrows)
			{
				// < value >: stepped triangles on the grid
				sArrows(vr, y, vw, ink);
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
		sWrap(about, sx + 8, sy + 188, sw - 16, 2, sDim);

		// the buttons
		const Item& cur = items[sel];
		std::string act = inGroups ? tr("Open") : cur.kind == Item::ACTION ? (armed == sel ? tr("Confirm") : tr(cur.label)) :
			cur.kind == Item::INFO ? "" : cur.key == "scrape" ? (scraping() ? "" : tr("Start")) : cur.kind == Item::PAGE ? tr("Open") : tr("Change");
		sChip("B", inGroups ? tr("Close") : tr("Back"), 8, 254);
		if (!act.empty()) sChip("A", act, 302 - sChipWidth("A", act), 254);
		be->endScreen();
	}

	// ---- the phone book: the keyboard (a search, a password, an account) ------------------------------------------
	const char* keyboardRow(int layer, int row);
	void keyboardFkey(int i, const char*& name, int& c0, int& span);
	int keyboardFkeyAt(int col);

	// the keys on the page: 10 a row 28 apart, 4 rows and the wide keys 26 apart, each 26 x 22
	static const float PX = 15, PP = 28, PY = 60, PR = 26, PW = 26, PH = 22;

	bool Settings::phoneBookTouch(float x, float y, int& row, int& col, int& button)
	{
		const float ux = x * s / u, uy = y * s / u;
		button = 0;
		if (uy >= 250) { button = ux < 155 ? -1 : 1; return true; }
		row = (int)std::floor((uy - PY) / PR);
		if (row < 0 || row > 4 || uy - PY - row * PR > PH) return false;
		col = (int)std::floor((ux - PX) / PP);
		return col >= 0 && col <= 9;
	}

	void Settings::renderPhoneBook()
	{
		const Color ink = sInk;
		be->beginScreen();
		sFill(0, 0, 310, 270, sDim);
		sImg("workshop.png", 0, 0);
		sImg("book.png", 4, 24);

		// the header: what is being typed for, and on the right what it finds (a search) or its length
		std::string title = entry == ENTRY_SEARCH ? tr("Phone book") : tr(entryTitle) + (entrySub.empty() ? "" : " " + entrySub);
		std::string n = std::to_string(pw.size());
		if (entry == ENTRY_SEARCH)
		{
			int m = pw.empty() || !searchCount ? -1 : searchCount(pw);
			n = m < 0 ? "" : m == 0 ? tr("No games") : std::to_string(m) + " " + tr(m == 1 ? "game" : "games");
		}
		float nw = sText(n, 0, 0, sPaper, true);
		title = sFit(title, 290 - nw - (nw > 0 ? 8 : 0));
		sText(title, 10, 6, ink);
		sText(title, 9, 5, sGold);
		sText(n, 301 - nw, 5, sPaper);

		// the line being written on, over the page's dotted rule; the cursor an ink block after it
		{
			const float room = 270;
			std::string t = pw;
			float tw = sText(t, 0, 0, ink, true);
			if (tw > room)
			{
				std::u32string w = rnds::utf8to32(pw);
				while (w.size() > 1 && tw > room) { w.erase(0, 1); t = "..." + utf8(w); tw = sText(t, 0, 0, ink, true); }
			}
			if (pw.empty())
			{
				tw = 0;
				sText(tr(entrySub.empty() || entry == ENTRY_SEARCH ? "Type a name" : entrySub), 18, 35, sMuted);
			}
			else sText(t, 15, 35, ink);
			sFill(16 + std::ceil(tw), 35, 5, 11, ink);
		}

		// the keys: index tabs, paper on ink, the selected one gold; a lit Shift or #+= dark
		auto key = [&](float x, float y, float w, const std::string& label, bool on, bool lit)
		{
			sFill(x + 1, y + 1, w, PH, Color(0, 0, 0, 0.3f));
			sFill(x, y, w, PH, ink);
			sFill(x + 1, y + 1, w - 2, PH - 2, on ? sGold : lit ? sDim : sPaper);
			sFill(x + 1, y + PH - 3, w - 2, 2, on ? Color(sGold.r * 0.82f, sGold.g * 0.62f, sGold.b * 0.3f, 1) : lit ? ink : sMuted);
			if (on) sFill(x + 1, y + 1, w - 2, 1, Color(1, 1, 1, 0.55f));
			float tw = sText(label, 0, 0, ink, true);
			sText(label, x + std::floor((w - tw) / 2), y + 4, lit && !on ? sPaper : ink);
		};
		for (int r = 0; r < 4; r++)
		{
			const char* row = keyboardRow(layer, r);
			for (int c = 0; c < 10; c++)
				key(PX + c * PP, PY + r * PR, PW, std::string(1, row[c]), r == krow && c == kcol, false);
		}
		const int fsel = krow == 4 ? keyboardFkeyAt(kcol) : -1;
		for (int i = 0; i < 5; i++)
		{
			const char* name; int c0, span;
			keyboardFkey(i, name, c0, span);
			std::string k = i == 1 && layer == 2 ? "abc" : i == 0 && layer == 1 ? "SHIFT" : tr(name);
			if (i == 4) k = tr(entry == ENTRY_WIFI ? "Join" : "Done");
			key(PX + c0 * PP, PY + 4 * PR, span * PP - 2, k, i == fsel, (i == 0 && layer == 1) || (i == 1 && layer == 2));
		}

		// under the keys: what the buttons do, or what went wrong
		std::string help = !message.empty() ? message
			: entry == ENTRY_SEARCH ? "Type part of a name. START looks it up: the games found open as a shelf of their own. An empty search takes the shelf away."
			: entry == ENTRY_WIFI ? "L for symbols, START joins. The password is kept in banana/wifi.conf on the card."
			: "L for symbols, START when it is typed. Accounts are kept in banana/accounts.conf on the card.";
		sWrap(tr(help), 15, 192, 280, 3, message.empty() ? sDim : sRed);

		// the buttons
		float x = 8;
		const std::pair<const char*, std::string> chips[3] = { { "B", tr(pw.empty() ? "Back" : "Delete") }, { "X", tr("Shift") }, { "Y", tr("Space") } };
		for (const auto& c : chips) { sChip(c.first, c.second, x, 254); x += sChipWidth(c.first, c.second) + 8; }
		std::string done = tr(entry == ENTRY_WIFI ? "Join" : "Done");
		sChip("START", done, 302 - sChipWidth("START", done), 254);
		be->endScreen();
	}

	// ---- the ticket: the pause menu over a frozen game -----------------------------------------------------------
	void Settings::renderTicket(const PauseView& v)
	{
		const Color ink = sInk;
		const float tx = 4, ty = 4, tw = 302, th = 262, cut = tx + 184;
		rememberCard(tx * u / s, ty * u / s, tw * u / s, th * u / s);
		sImg("ticket.png", tx, ty);

		// the red band: the game, and on the right the battery, the clock and the time played
		float stw = sText(v.status, 0, 0, sPaper, true);
		std::string title = sFit(v.title.empty() ? tr("Paused") : v.title, tw - 20 - stw - (stw > 0 ? 8 : 0));
		sText(title, tx + 8, ty + 6, ink);
		sText(title, tx + 7, ty + 5, sPaper);
		if (stw > 0) sText(v.status, tx + tw - 7 - stw, ty + 5, sPaper);

		// the stub: the rows, the selected one gold
		const float rx = tx + 5, rw = cut - 4 - rx, top = ty + 31, pitch = 18;
		const int vis = 11, n = (int)v.rows.size();
		int first = std::max(0, std::min(v.sel - vis / 2, n - vis));
		for (int i = first; i < std::min(n, first + vis); i++)
		{
			const PauseRow& r = v.rows[i];
			const bool on = i == v.sel, armed = i == v.armed;
			const float y = top + (i - first) * pitch;
			if (on)
			{
				sFill(rx, y - 2, rw, 15, sGold);
				sFill(rx, y - 2, rw, 1, Color(1, 1, 1, 0.55f));
			}
			const Color c = armed ? sRed : on ? ink : sDim;
			std::string val = armed || r.value.empty() ? "" : tr(r.value);
			const bool arrows = r.arrows && on && !val.empty();
			float vw = val.empty() ? 0 : sText(val, 0, 0, c, true);
			float vroom = vw > 0 ? vw + (arrows ? 13 : 0) + 6 : 0;
			sText(sFit(armed ? tr("Press A again") : tr(r.label), rw - 10 - vroom), rx + 4, y, c);
			if (val.empty()) continue;
			const float vr = rx + rw - 4;
			if (arrows) { sArrows(vr, y, vw, ink); sText(val, vr - 6 - vw, y, c); }
			else sText(val, vr - vw, y, c);
		}

		// the other half: the picture in its frame (fitted, aspect kept), its caption and a short list
		const float fx = tx + 192, fy = ty + 32, fw = 102, fh = 78;
		if (!v.picture.empty())
		{
			struct stat st;
			std::string key = "pause:" + v.picture;
			if (stat(v.picture.c_str(), &st) == 0) key += ":" + std::to_string((long long)st.st_mtime);	// a new save redraws
			auto it = cache.find(key);
			if (it == cache.end())
			{
				rnds::Bitmap bm = rnds::decodeFile(v.picture);
				Cached c;
				if (!bm.empty()) c.tex = be->upload(bm, true);
				it = cache.emplace(key, c).first;
			}
			if (it->second.tex && it->second.tex->w > 0 && it->second.tex->h > 0)
			{
				float iw = (float)it->second.tex->w, ih = (float)it->second.tex->h;
				float k = std::min((fw - 2) * u / iw, (fh - 2) * u / ih);
				float dw = std::round(iw * k), dh = std::round(ih * k);
				be->draw(it->second.tex, std::round(fx * u + (fw * u - dw) / 2), std::round(fy * u + (fh * u - dh) / 2), dw, dh, Color(1, 1, 1, 1));
			}
		}
		float y = fy + fh + 6;
		if (!v.caption.empty()) { sWrap(v.caption, fx, y, fw, 2, ink); y += 28; }
		for (size_t i = 0; i < v.lines.size() && i < 7; i++, y += 12) sText(sFit(v.lines[i], fw), fx, y, sDim);

		// the dark band: the buttons
		const float by = ty + th - 17;
		sChip("A", tr("Choose"), tx + 6, by);
		const bool change = v.sel >= 0 && v.sel < n && v.rows[v.sel].arrows;
		if (change) sChip("<>", tr("Change"), tx + 12 + sChipWidth("A", tr("Choose")), by);
		sChip("B", tr("Resume"), tx + tw - 6 - sChipWidth("B", tr("Resume")), by);
	}
}
