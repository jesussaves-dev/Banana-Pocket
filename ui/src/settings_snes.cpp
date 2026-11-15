// banana-ui: Settings in the SNES style -- the workshop, its pages on the ledger (Wi-Fi, Updates, setup, Save states),
// the phone book (the keyboard), the ticket (the pause menu), and the pill and notices over any screen.
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

	// one of the theme's baked panels, a texel a unit (k units a texel: the notice's banana)
	void Settings::sImg(const std::string& file, float x, float y)
	{
		sImgScaled(file, x, y, 1);
	}

	void Settings::sImgScaled(const std::string& file, float x, float y, float k)
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
		if (it->second.tex) be->draw(it->second.tex, x * u, y * u, it->second.tex->w * u * k, it->second.tex->h * u * k, Color(1, 1, 1, 1));
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

	// ---- the ledger: the workshop's own pages -------------------------------------------------------------------
	// the sheet's place on the pegboard, and its rows: LEDGER_VIS of them 16 apart under the status line
	static const float LX = 4, LY = 24, LW = 302, LROW = LY + 24, LPITCH = 16;

	int Settings::ledgerFirst(int sel, int n)
	{
		return std::max(0, std::min(sel - LEDGER_VIS / 2, n - LEDGER_VIS));
	}

	int Settings::ledgerRowAt(float uy, int sel, int n) const
	{
		if (uy < LROW - 2) return -1;
		int r = (int)std::floor((uy - (LROW - 2)) / LPITCH);
		int i = ledgerFirst(sel, n) + r;
		return r < LEDGER_VIS && i < n ? i : -1;
	}

	void Settings::renderLedger(const std::string& title, const std::string& count, const std::string& status, const std::vector<SRow>& rows,
		int sel, const std::string& empty, const std::string& help, bool helpIsMessage, const std::vector<SChip>& left, const SChip& right)
	{
		const Color ink = sInk;
		be->beginScreen();
		sFill(0, 0, 310, 270, sDim);
		sImg("workshop.png", 0, 0);
		// the header: the page, and on the right how many
		float cw = sText(count, 0, 0, sPaper, true);
		std::string t = sFit(title, 290 - cw - (cw > 0 ? 8 : 0));
		sText(t, 10, 6, ink);
		sText(t, 9, 5, sGold);
		if (cw > 0) sText(count, 301 - cw, 5, sPaper);

		// the sheet: where things stand over the rule, the rows under it
		sImg("ledger.png", LX, LY);
		sText(sFit(status, LW - 16), LX + 8, LY + 5, ink);
		const int n = (int)rows.size(), first = ledgerFirst(sel, n);
		for (int i = first; i < std::min(n, first + LEDGER_VIS); i++)
		{
			const SRow& r = rows[i];
			const float y = LROW + (i - first) * LPITCH;
			const bool on = i == sel;
			if (on)
			{
				sFill(LX + 3, y - 2, LW - 6, 15, sGold);
				sFill(LX + 3, y - 2, LW - 6, 1, Color(1, 1, 1, 0.55f));
			}
			const Color c = r.accent ? sRed : on ? ink : sDim, faint = on ? Color(ink.r, ink.g, ink.b, 0.25f) : sMuted;
			float rx = LX + LW - 8;
			if (r.bars >= 0)
			{
				// the signal: three steps, the lit ones in ink
				for (int k = 0; k < 3; k++) sFill(rx - 11 + k * 4, y + 8 - 3 * k, 3, 3 + 3 * k, k < r.bars ? ink : faint);
				rx -= 17;
			}
			if (r.lock)
			{
				// a padlock 7 x 9: the shackle, then the body
				sFill(rx - 5, y + 1, 3, 1, c); sFill(rx - 6, y + 2, 1, 3, c); sFill(rx - 2, y + 2, 1, 3, c);
				sFill(rx - 7, y + 5, 7, 5, c);
				rx -= 13;
			}
			float vroom = 0;
			if (!r.value.empty())
			{
				const bool arrows = r.arrows && on;
				std::string v = sFit(r.value, 150 - (arrows ? 13 : 0));
				float vw = sText(v, 0, 0, c, true);
				if (arrows) { sArrows(rx, y, vw, ink); sText(v, rx - 6 - vw, y, c); vroom = vw + 13; }
				else { sText(v, rx - vw, y, c); vroom = vw; }
				vroom += 8;
			}
			sText(sFit(r.label, rx - vroom - (LX + 8)), LX + 8, y, on ? ink : sDim);
		}
		if (rows.empty() && !empty.empty()) sWrap(empty, LX + 8, LROW, LW - 16, 3, sDim);
		// a page that scrolls: a stepped mark at the end of the rule where there is more
		if (first > 0) for (int k = 0; k < 3; k++) sFill(LX + LW - 12 - k, LY + 14 - k, 1 + 2 * k, 1, sDim);
		if (first + LEDGER_VIS < n) for (int k = 0; k < 3; k++) sFill(LX + LW - 12 - k, LY + 161 + k, 1 + 2 * k, 1, sDim);

		// under the rule: what A does here, or what went wrong
		sWrap(help, LX + 8, LY + 171, LW - 16, 4, helpIsMessage ? sRed : sDim);

		// the buttons
		float x = 8;
		for (const SChip& c : left) { sChip(c.key, c.label, x, 254); x += sChipWidth(c.key, c.label) + 8; }
		if (!right.key.empty()) sChip(right.key, right.label, 302 - sChipWidth(right.key, right.label), 254);
		be->endScreen();
	}

	// ---- touch on the SNES pages ----------------------------------------------------------------------------------
	Settings::Result Settings::snesTouch(float cx, float cy)
	{
		const float ux = downX * s / u, uy = downY * s / u, uup = cy * s / u;
		(void)cx;
		const bool swipe = std::fabs(uup - uy) > 10;
		// the button bar: B on the left half, A on the right
		if (uy >= 250)
		{
			const Key k = ux < 155 ? Key::B : Key::A;
			switch (page)
			{
			case NETWORKS: return pressNetworks(k);
			case UPDATES: return pressUpdates(k);
			case SETUP: return pressSetup(k);
			case STATES: return pressStates(k);
			default: break;
			}
			if (inGroups) return pressGroups(k);
			return k == Key::B ? press(Key::B) : change(1);
		}
		if (page != LIST)
		{
			int n = 0, *cur = nullptr;
			Result (Settings::*pressIt)(Key) = nullptr;
			switch (page)
			{
			case NETWORKS: n = (int)nets.size(); cur = &nsel; pressIt = &Settings::pressNetworks; break;
			case UPDATES: n = (int)pkgs.size() + 1; cur = &usel; pressIt = &Settings::pressUpdates; break;
			case SETUP: n = 5; cur = &wsel; pressIt = &Settings::pressSetup; break;
			case STATES: n = (int)states.size(); cur = &ssel; pressIt = &Settings::pressStates; break;
			default: return NONE;
			}
			if (n <= 0) return NONE;
			auto pick = [&](int i) {
				*cur = i;
				armed = -1;
				if (page == NETWORKS) ntop = std::max(0, std::min(nsel, std::max(0, n - ROWS)));	// (the Classic window, kept in step)
				if (page == SETUP) message.clear();
				return MOVED;
			};
			if (swipe) return pick(std::max(0, std::min(n - 1, *cur + (uup < uy ? LEDGER_VIS : -LEDGER_VIS))));
			int i = ux >= LX && ux < LX + LW ? ledgerRowAt(uy, *cur, n) : -1;
			if (i < 0) return NONE;
			if (i != *cur) return pick(i);
			return (this->*pressIt)(Key::A);
		}

		// the workshop: a drawer opens its group (an open one shuts), a row on the sheet is chosen, then changed
		const int ng = std::min<int>((int)groups.size(), 8);
		const int g = std::max(0, std::min(inGroups ? gsel : groupOf(sel), ng - 1));
		const int gb = groupStart(g), ge = groupEnd(g);
		if (ux < 127 && !swipe)
		{
			int i = (int)std::floor((uy - 26) / 28);
			if (uy < 26 || i >= ng || uy - 26 - i * 28 > 26) return NONE;
			if (inGroups && i != gsel) { gsel = i; return MOVED; }
			if (!inGroups && i == g) return press(Key::B);
			inGroups = true; gsel = i;
			return pressGroups(Key::A);
		}
		const float sx = 132, sy = 24, sw = 170;
		if (ux < sx || ux >= sx + sw) return NONE;
		if (swipe)
		{
			if (inGroups) return NONE;
			sel = std::max(gb, std::min(ge - 1, sel + (uup < uy ? 10 : -10)));
			armed = -1;
			return MOVED;
		}
		const int vis = 10;
		int first = inGroups ? gb : std::max(gb, std::min(sel - vis / 2, ge - vis));
		first = std::max(gb, first);
		int r = (int)std::floor((uy - (sy + 22)) / 16), i = first + r;
		if (uy < sy + 22 || r >= vis || i >= ge) return NONE;
		if (inGroups) { pressGroups(Key::A); sel = i; armed = -1; return MOVED; }
		if (i != sel) { sel = i; armed = -1; return MOVED; }
		// the chosen row: on a value, its left half steps back and the right forward; a page or an action just goes
		const Item::Kind kd = items[i].kind;
		const bool stepped = kd == Item::CHOICE || kd == Item::BRIGHTNESS || kd == Item::VOLUME || kd == Item::PERCENT;
		return change(stepped && ux < sx + sw / 2 ? -1 : 1);
	}

	// ---- over any screen: the volume/brightness pill and the battery's notices -------------------------------------
	void Settings::levelSnes(const std::string& label, int percent)
	{
		static const char* const VOL[12] = { "............", ".....#......", "....##...#..", "...###....#.", "####.#..#..#", "#..#.#...#.#",
			"#..#.#...#.#", "####.#..#..#", "...###....#.", "....##...#..", ".....#......", "............" };
		static const char* const MUTE[12] = { "............", ".....#......", "....##......", "...###.#...#", "####.#..#.#.", "#..#.#...#..",
			"#..#.#...#..", "####.#..#.#.", "...###.#...#", "....##......", ".....#......", "............" };
		static const char* const SUN[12] = { ".....##.....", ".#...##...#.", "..#......#..", "....####....", "...######...", "##.######.##",
			"##.######.##", "...######...", "....####....", "..#......#..", ".#...##...#.", ".....##....." };
		percent = std::min(100, std::max(0, percent));
		const bool sun = label == "Brightness", mute = !sun && percent <= 0;
		const float w = 124, h = 22, x0 = (310 - w) / 2, y0 = 4;
		rememberCard(x0 * u / s, y0 * u / s, w * u / s, h * u / s);
		sImg("pill.png", x0, y0);
		const char* const* ic = sun ? SUN : mute ? MUTE : VOL;
		for (int r = 0; r < 12; r++)
			for (int c = 0; c < 12; c++)
				if (ic[r][c] == '#') sFill(x0 + 7 + c, y0 + 5 + r, 1, 1, mute ? sRed : sPaper);
		// ten steps like the workshop's sliders, then the percent
		for (int k = 0; k < 10; k++)
			sFill(x0 + 25 + k * 5, y0 + 6, 4, 10, k < (percent + 5) / 10 ? sGold : Color(sPaper.r, sPaper.g, sPaper.b, 0.2f));
		std::string t = std::to_string(percent) + "%";
		sText(t, x0 + w - 7 - sText(t, 0, 0, sGold, true), y0 + 5, sGold);
	}

	void Settings::noticeSnes(const std::string& title, const std::string& body, int percent, bool charging)
	{
		const float x0 = 14, y0 = 196, w = 282, h = 48;
		rememberCard(x0 * u / s, y0 * u / s, w * u / s, h * u / s);
		sImg("notice.png", x0, y0);
		// the street's banana at twice its size, as ripe as the charge (the same stages as the status bar's)
		int stage = percent < 0 || percent >= 70 ? 0 : percent >= 45 ? 1 : percent >= 25 ? 2 : percent >= 10 ? 3 : 4;
		sImgScaled("batt" + std::to_string(stage) + ".png", x0 + 11, y0 + 14, 2);		// 36 x 20, centred left of the text
		if (charging) sImgScaled("bolt.png", x0 + 22, y0 + 14, 2);				// 14 x 20, centred over it
		const float tx = x0 + 58, tw = w - 58 - 8;
		std::string t = sFit(tr(title), tw);
		sText(t, tx + 1, y0 + 7, sInk);
		sText(t, tx, y0 + 6, percent >= 0 && percent <= 10 && !charging ? Color(1.0f, 0.42f, 0.42f, 1) : sGold);
		sWrap(tr(body), tx, y0 + 21, tw, 2, sPaper);
	}
}
