// banana-ui: Settings in the Genesis style (Banana Blast) -- the Options screen, its pages, name entry (the keyboard),
// the pause panel, and the pill and notices over any screen. The SNES style's grid, page model, input and touch
// (settings_snes.cpp) with `snes` set; only the drawing differs. Everything sits on true black (the AMOLED's pixels off)
// in 9-bit colour from the theme's vars, in the leaning Blast face at 12 outlined in black, with the chrome title face
// at 24 banded one stop a row. No baked art and no blends, except the pause's shadow: the game behind at half level.
// Nothing blinks: a screen here is drawn when it changes, not on a timer.
#include "settings.h"
#include "i18n.h"
#include "raster.h"
#include <algorithm>
#include <cmath>
#include <sys/stat.h>

namespace banana
{
	using rnds::Color;

	static const Color VOID(0, 0, 0, 1);

	// the era's menus are capitals; only ASCII is raised (a name in another script stays as it is)
	static std::string up(std::string t)
	{
		for (char& ch : t)
			if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
		return t;
	}

	float Settings::gText(const std::string& t, float x, float y, const Color& c)
	{
		static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 }, oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
		for (int k = 0; k < 8; k++) sText(t, x + ox[k], y + oy[k], VOID);
		return sText(t, x, y, c);
	}

	// the chrome title: BananaBlastTitle at 24, outlined in black, then once a unit row through a clip in that row's stop
	float Settings::gChromeText(const std::string& t, float x, float y, bool measureOnly)
	{
		if (t.empty()) return 0;
		if (!BT) BT = rnds::Font::load(bananaDir + "/fonts/BananaBlastTitle.ttf");
		if (!BT) return measureOnly ? sText(t, 0, 0, gInk, true) : gText(t, x, y + 6, gChrome[0]);	// (no face: the small one)
		std::string key = "gttl:" + t;
		auto it = cache.find(key);
		if (it == cache.end())
		{
			rnds::TextStyle st;
			st.font = BT; st.size = 24;
			rnds::TextLine l = rnds::layoutLine(rnds::utf8to32(t), st);
			rnds::TextBitmap tb = rnds::renderLine(l, st, u, st.font->ascent(24));
			Cached cc;
			if (!tb.bmp.empty()) cc.tex = be->upload(tb.bmp, true);
			cc.ox = tb.padX; cc.oy = tb.padY; cc.w = l.width;
			it = cache.emplace(key, cc).first;
		}
		const Cached& cc = it->second;
		if (measureOnly || !cc.tex) return cc.w;
		const float dx = std::floor(x * u) - cc.ox, dy = std::floor(y * u) - cc.oy, tw = (float)cc.tex->w, th = (float)cc.tex->h;
		static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 }, oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
		for (int k = 0; k < 8; k++) be->draw(cc.tex, dx + ox[k] * u, dy + oy[k] * u, tw, th, VOID);
		for (int r = 0; r < 24; r++)
		{
			be->pushClip(0, std::floor((y + r) * u), 310 * u, u);
			be->draw(cc.tex, dx, dy, tw, th, gChrome[r]);
			be->popClip();
		}
		return cc.w;
	}

	// a button and what it does: the key in yellow on a black chip with a hairline, the label in white (the theme's pill)
	float Settings::gChipWidth(const std::string& key, const std::string& label)
	{
		return sText(key, 0, 0, gInk, true) + 4 + 3 + sText(label, 0, 0, gInk, true);
	}

	void Settings::gChip(const std::string& key, const std::string& label, float x, float y)
	{
		float kw = sText(key, 0, 0, gInk, true) + 4;
		sFill(x, y, kw, 13, VOID);
		gBox(x, y, kw, 13, gLine);
		sText(key, x + 2, y, gLabel);
		gText(label, x + kw + 3, y, gInk);
	}

	void Settings::gBox(float x, float y, float w, float h, const Color& c, float t)
	{
		sFill(x, y, w, t, c);
		sFill(x, y + h - t, w, t, c);
		sFill(x, y, t, h, c);
		sFill(x + w - t, y, t, h, c);
	}

	// the selection's pointer: a stepped triangle, 3 wide and 7 tall, pointing right
	void Settings::gCursor(float x, float y, const Color& c)
	{
		for (int k = 0; k < 3; k++) sFill(x + k, y + 2 + k, 1, 7 - 2 * k, c);
	}

	// ---- Options: the groups as one list, then the open group's rows (label in white, value in yellow) ------------
	// the rows: OVIS of them OP apart from OY (genesisTouch finds them the same way)
	static const float OY = 54, OP = 18;
	static const int OVIS = 9;

	void Settings::renderOptions()
	{
		const int ng = (int)groups.size();
		const int g = std::max(0, std::min(inGroups ? gsel : groupOf(sel), ng - 1));
		const int gb = groupStart(g), ge = groupEnd(g);
		be->beginScreen();
		sFill(0, 0, 310, 270, VOID);

		// the title in chrome over two rules, then where this is and how far down
		std::string title = up(tr("Options"));
		gChromeText(title, std::floor((310 - gChromeText(title, 0, 0, true)) / 2), 2);
		sFill(40, 28, 230, 1, gMag);
		sFill(60, 30, 190, 1, gCyan);
		const int n = inGroups ? ng : ge - gb, cur = inGroups ? gsel : sel - gb;
		std::string count = std::to_string(cur + 1) + "/" + std::to_string(n);
		float cw = sText(count, 0, 0, gDim, true);
		gText(sFit(up(inGroups ? tr("Settings") : tr(groups[g])), 280 - cw), 12, 36, gLabel);
		sText(count, 298 - cw, 36, gDim);

		const int first = std::max(0, std::min(cur - OVIS / 2, n - OVIS));
		for (int i = first; i < std::min(n, first + OVIS); i++)
		{
			const float y = OY + (i - first) * OP, vr = 298;
			const bool on = i == cur;
			if (on) gCursor(12, y, gLabel);
			if (inGroups)
			{
				std::string v = std::to_string(groupEnd(i) - groupStart(i));
				float vw = sText(v, 0, 0, gDim, true);
				gText(sFit(up(tr(groups[i])), vr - vw - 6 - 24), 24, y, on ? gLabel : gInk);
				sText(v, vr - vw, y, gDim);
				continue;
			}
			const Item& it = items[gb + i];
			std::string v = rowValue(gb + i);
			if (!v.empty()) v = up(tr(v));
			const bool slider = it.kind == Item::BRIGHTNESS || it.kind == Item::VOLUME || it.kind == Item::PERCENT;
			const bool arrows = it.kind == Item::CHOICE && on;
			float valueW = slider ? 50 : v.empty() ? 0 : std::min(130.0f, sText(v, 0, 0, gLabel, true) + (arrows ? 13 : 0));
			gText(sFit(up(tr(it.label)), vr - 24 - valueW - (valueW > 0 ? 8 : 0)), 24, y, on ? gLabel : gInk);
			if (slider)
			{
				int pct = atoi(it.values[it.at].c_str());
				if (it.kind == Item::BRIGHTNESS) pct /= 10;
				pct = std::min(100, std::max(0, pct));
				for (int k = 0; k < 10; k++) sFill(vr - 50 + k * 5, y + 2, 4, 8, k < (pct + 5) / 10 ? gLabel : gLine);
				continue;
			}
			if (v.empty()) continue;
			std::string vs = sFit(v, 130 - (arrows ? 13 : 0));
			float vw = sText(vs, 0, 0, gLabel, true);
			if (arrows) { sArrows(vr, y, vw, gLabel); gText(vs, vr - 6 - vw, y, gLabel); }
			else gText(vs, vr - vw, y, it.kind == Item::INFO ? gDim : gLabel);
		}
		// more above or below: a stepped mark at the list's right end
		if (first > 0) for (int k = 0; k < 3; k++) sFill(300 - k, OY - 6 + k, 1 + 2 * k, 1, gDim);
		if (first + OVIS < n) for (int k = 0; k < 3; k++) sFill(300 - k, OY + OVIS * OP - 2 - k, 1 + 2 * k, 1, gDim);

		// what the chosen group holds, or what the selected setting does
		std::string about;
		if (inGroups) { for (int i = gb; i < ge; i++) about += (about.empty() ? "" : ", ") + tr(items[i].label); }
		else about = rowHelp();
		sFill(12, 218, 286, 1, gLine);
		sWrap(about, 12, 224, 286, 2, gDim);

		const Item& curIt = items[sel];
		std::string act = inGroups ? tr("Open") : curIt.kind == Item::ACTION ? (armed == sel ? tr("Confirm") : tr(curIt.label)) :
			curIt.kind == Item::INFO ? "" : curIt.key == "scrape" ? (scraping() ? "" : tr("Start")) : curIt.kind == Item::PAGE ? tr("Open") : tr("Change");
		gChip("B", inGroups ? tr("Close") : tr("Back"), 8, 254);
		if (!act.empty()) gChip("A", act, 302 - gChipWidth("A", act), 254);
		be->endScreen();
	}

	Settings::Result Settings::genesisTouch(float cx, float cy)
	{
		const float ux = downX * s / u, uy = downY * s / u, uup = cy * s / u;
		(void)cx;
		const bool swipe = std::fabs(uup - uy) > 10;
		if (uy >= 250)
		{
			const Key k = ux < 155 ? Key::B : Key::A;
			if (inGroups) return pressGroups(k);
			return k == Key::B ? press(Key::B) : change(1);
		}
		const int ng = (int)groups.size();
		const int g = std::max(0, std::min(inGroups ? gsel : groupOf(sel), ng - 1));
		const int gb = groupStart(g), ge = groupEnd(g);
		const int n = inGroups ? ng : ge - gb, cur = inGroups ? gsel : sel - gb;
		if (n <= 0) return NONE;
		if (swipe)
		{
			const int to = std::max(0, std::min(n - 1, cur + (uup < uy ? OVIS : -OVIS)));
			if (inGroups) gsel = to; else { sel = gb + to; armed = -1; }
			return MOVED;
		}
		const int first = std::max(0, std::min(cur - OVIS / 2, n - OVIS));
		const int r = (int)std::floor((uy - (OY - 3)) / OP), i = first + r;
		if (uy < OY - 3 || r >= OVIS || i >= n) return NONE;
		if (inGroups)
		{
			if (i != gsel) { gsel = i; return MOVED; }
			return pressGroups(Key::A);
		}
		if (gb + i != sel) { sel = gb + i; armed = -1; return MOVED; }
		// the chosen row: on a value, its left half steps back and the right forward; a page or an action just goes
		const Item::Kind kd = items[sel].kind;
		const bool stepped = kd == Item::CHOICE || kd == Item::BRIGHTNESS || kd == Item::VOLUME || kd == Item::PERCENT;
		return change(stepped && ux < 155 ? -1 : 1);
	}

	// ---- the Options pages (Wi-Fi, Updates, setup, Save states): the ledger's places, so snesTouch finds the rows ----
	static const float LX = 4, LY = 24, LW = 302, LROW = LY + 24, LPITCH = 16;

	void Settings::ledgerBlast(const std::string& title, const std::string& count, const std::string& status, const std::vector<SRow>& rows,
		int sel, const std::string& empty, const std::string& help, bool helpIsMessage, const std::vector<SChip>& left, const SChip& right)
	{
		be->beginScreen();
		sFill(0, 0, 310, 270, VOID);
		float cw = sText(count, 0, 0, gDim, true);
		gText(sFit(up(title), 290 - cw - (cw > 0 ? 8 : 0)), 9, 5, gLabel);
		if (cw > 0) sText(count, 301 - cw, 5, gDim);

		// where things stand over a hairline, the rows under it
		sText(sFit(status, LW - 16), LX + 8, LY + 5, gInk);
		sFill(LX + 4, LY + 19, LW - 8, 1, gMag);
		const int n = (int)rows.size(), first = ledgerFirst(sel, n);
		for (int i = first; i < std::min(n, first + LEDGER_VIS); i++)
		{
			const SRow& r = rows[i];
			const float y = LROW + (i - first) * LPITCH;
			const bool on = i == sel;
			if (on) gCursor(LX + 1, y, gLabel);
			const Color c = r.accent ? gRed : on ? gLabel : gInk;
			float rx = LX + LW - 8;
			if (r.bars >= 0)
			{
				for (int k = 0; k < 3; k++) sFill(rx - 11 + k * 4, y + 8 - 3 * k, 3, 3 + 3 * k, k < r.bars ? gCyan : gLine);
				rx -= 17;
			}
			if (r.lock)
			{
				sFill(rx - 5, y + 1, 3, 1, c); sFill(rx - 6, y + 2, 1, 3, c); sFill(rx - 2, y + 2, 1, 3, c);
				sFill(rx - 7, y + 5, 7, 5, c);
				rx -= 13;
			}
			float vroom = 0;
			if (!r.value.empty())
			{
				const bool arrows = r.arrows && on;
				std::string v = sFit(r.value, 150 - (arrows ? 13 : 0));
				float vw = sText(v, 0, 0, gLabel, true);
				const Color vc = r.accent ? gRed : gLabel;
				if (arrows) { sArrows(rx, y, vw, gLabel); gText(v, rx - 6 - vw, y, vc); vroom = vw + 13; }
				else { gText(v, rx - vw, y, on ? vc : r.accent ? gRed : gDim); vroom = vw; }
				vroom += 8;
			}
			gText(sFit(r.label, rx - vroom - (LX + 8)), LX + 8, y, c);
		}
		if (rows.empty() && !empty.empty()) sWrap(empty, LX + 8, LROW, LW - 16, 3, gDim);
		if (first > 0) for (int k = 0; k < 3; k++) sFill(LX + LW - 12 - k, LY + 14 - k, 1 + 2 * k, 1, gDim);
		if (first + LEDGER_VIS < n) for (int k = 0; k < 3; k++) sFill(LX + LW - 12 - k, LY + 161 + k, 1 + 2 * k, 1, gDim);

		sFill(LX + 4, LY + 166, LW - 8, 1, gLine);
		sWrap(help, LX + 8, LY + 171, LW - 16, 4, helpIsMessage ? gRed : gDim);

		float x = 8;
		for (const SChip& c : left) { gChip(c.key, c.label, x, 254); x += gChipWidth(c.key, c.label) + 8; }
		if (!right.key.empty()) gChip(right.key, right.label, 302 - gChipWidth(right.key, right.label), 254);
		be->endScreen();
	}

	// ---- name entry: the keyboard as an arcade's high-score grid ----------------------------------------------------
	// the phone book's places (phoneBookTouch): 10 a row 28 apart, 4 rows and the wide keys 26 apart, each 26 x 22
	const char* keyboardRow(int layer, int row);
	void keyboardFkey(int i, const char*& name, int& c0, int& span);
	int keyboardFkeyAt(int col);
	static const float PX = 15, PP = 28, PY = 60, PR = 26, PW = 26, PH = 22;

	void Settings::renderNameEntry()
	{
		be->beginScreen();
		sFill(0, 0, 310, 270, VOID);

		// the title in chrome (or, too long for it, in yellow), and on the right what the search finds
		std::string title = up(entry == ENTRY_SEARCH ? tr("Search") : tr(entryTitle) + (entrySub.empty() ? "" : " " + entrySub));
		std::string n;
		if (entry == ENTRY_SEARCH)
		{
			int m = pw.empty() || !searchCount ? -1 : searchCount(pw);
			n = m < 0 ? "" : m == 0 ? tr("No games") : std::to_string(m) + " " + tr(m == 1 ? "game" : "games");
		}
		else n = std::to_string(pw.size());
		n = up(n);
		float nw = sText(n, 0, 0, gDim, true);
		const float room = 294 - nw - (nw > 0 ? 8 : 0);
		if (gChromeText(title, 0, 0, true) <= room) gChromeText(title, 8, 2);
		else gText(sFit(title, room), 8, 10, gLabel);
		sText(n, 302 - nw, 10, gDim);

		// what has been typed, in a box: white, a yellow underline for the cursor after it
		gBox(10, 30, 290, 24, gMag, 2);
		{
			const float room2 = 270;
			std::string t = pw;
			float tw = sText(t, 0, 0, gInk, true);
			if (tw > room2)
			{
				std::u32string w = rnds::utf8to32(pw);
				while (w.size() > 1 && tw > room2)
				{
					w.erase(0, 1);
					std::string c;
					for (char32_t ch : w)
					{
						if (ch < 0x80) c += (char)ch;
						else if (ch < 0x800) { c += (char)(0xc0 | (ch >> 6)); c += (char)(0x80 | (ch & 0x3f)); }
						else { c += (char)(0xe0 | (ch >> 12)); c += (char)(0x80 | ((ch >> 6) & 0x3f)); c += (char)(0x80 | (ch & 0x3f)); }
					}
					t = "..." + c;
					tw = sText(t, 0, 0, gInk, true);
				}
			}
			if (pw.empty())
			{
				tw = 0;
				sText(up(tr(entrySub.empty() || entry == ENTRY_SEARCH ? "Type a name" : entrySub)), 22, 36, gDim);
			}
			else sText(t, 16, 36, gInk);
			sFill(16 + std::ceil(tw) + (pw.empty() ? 0 : 1), 48, 6, 2, gLabel);
		}

		// the letters: bare glyphs on black, the chosen one yellow in a yellow frame; the wide keys in cyan, a lit one magenta
		auto key = [&](float x, float y, float w, const std::string& label, bool on, bool lit, bool wide)
		{
			if (on) gBox(x, y, w, PH, gLabel);
			float tw = sText(label, 0, 0, gInk, true);
			const Color c = on ? gLabel : lit ? gMag : wide ? gCyan : gInk;
			sText(label, x + std::floor((w - tw) / 2), y + 5, c);
		};
		for (int r = 0; r < 4; r++)
		{
			const char* row = keyboardRow(layer, r);
			for (int c = 0; c < 10; c++)
				key(PX + c * PP, PY + r * PR, PW, std::string(1, row[c]), r == krow && c == kcol, false, false);
		}
		const int fsel = krow == 4 ? keyboardFkeyAt(kcol) : -1;
		for (int i = 0; i < 5; i++)
		{
			const char* name; int c0, span;
			keyboardFkey(i, name, c0, span);
			std::string k = i == 1 && layer == 2 ? "abc" : i == 0 && layer == 1 ? "SHIFT" : tr(name);
			if (i == 4) k = tr(entry == ENTRY_WIFI ? "Join" : "Done");
			key(PX + c0 * PP, PY + 4 * PR, span * PP - 2, up(k), i == fsel, (i == 0 && layer == 1) || (i == 1 && layer == 2), true);
		}

		std::string help = !message.empty() ? message
			: entry == ENTRY_SEARCH ? "Type part of a name. START looks it up: the games found open as a shelf of their own. An empty search takes the shelf away."
			: entry == ENTRY_WIFI ? "L for symbols, START joins. The password is kept in banana/wifi.conf on the card."
			: "L for symbols, START when it is typed. Accounts are kept in banana/accounts.conf on the card.";
		sFill(12, 188, 286, 1, gLine);
		sWrap(tr(help), 15, 194, 280, 3, message.empty() ? gDim : gRed);

		float x = 8;
		const std::pair<const char*, std::string> chips[3] = { { "B", tr(pw.empty() ? "Back" : "Delete") }, { "X", tr("Shift") }, { "Y", tr("Space") } };
		for (const auto& c : chips) { gChip(c.first, c.second, x, 254); x += gChipWidth(c.first, c.second) + 8; }
		std::string done = tr(entry == ENTRY_WIFI ? "Join" : "Done");
		gChip("START", done, 302 - gChipWidth("START", done), 254);
		be->endScreen();
	}

	// ---- the pause panel: the game in shadow (every level halved) under a black panel, PAUSE in chrome -------------
	void Settings::pauseBlast(const PauseView& v)
	{
		// the whole screen is the card: half-black over the game is the Mega Drive's shadow mode
		rememberCard(0, 0, 310 * u / s, 270 * u / s);
		be->fill(0, 0, 310 * u, 270 * u, Color(0, 0, 0, 0.5f));
		const float px = 16, py = 10, pw2 = 278, ph = 250, cut = px + 166;
		sFill(px, py, pw2, ph, VOID);
		gBox(px, py, pw2, ph, gMag, 2);
		std::string pause = up(tr("Pause"));
		gChromeText(pause, px + std::floor((pw2 - gChromeText(pause, 0, 0, true)) / 2), py + 3);

		// the game and, on the right, the battery, the clock and the time played
		float stw = sText(v.status, 0, 0, gDim, true);
		std::string title = sFit(up(v.title.empty() ? tr("Paused") : v.title), pw2 - 20 - stw - (stw > 0 ? 8 : 0));
		gText(title, px + 8, py + 30, gInk);
		if (stw > 0) sText(v.status, px + pw2 - 8 - stw, py + 30, gDim);
		sFill(px + 6, py + 44, pw2 - 12, 1, gCyan);

		// the rows, the selected one yellow behind the pointer; one asking again in red
		const float rx = px + 8, top = py + 52, pitch = 16;
		const int vis = 11, n = (int)v.rows.size();
		int first = std::max(0, std::min(v.sel - vis / 2, n - vis));
		for (int i = first; i < std::min(n, first + vis); i++)
		{
			const PauseRow& r = v.rows[i];
			const bool on = i == v.sel, armed2 = i == v.armed;
			const float y = top + (i - first) * pitch;
			if (on) gCursor(rx, y, gLabel);
			const Color c = armed2 ? gRed : on ? gLabel : gInk;
			std::string val = armed2 || r.value.empty() ? "" : up(tr(r.value));
			const bool arrows = r.arrows && on && !val.empty();
			float vw = val.empty() ? 0 : sText(val, 0, 0, c, true);
			float vroom = vw > 0 ? vw + (arrows ? 13 : 0) + 6 : 0;
			const float vr = cut - 6;
			gText(sFit(up(armed2 ? tr("Press A again") : tr(r.label)), vr - (rx + 10) - vroom), rx + 10, y, c);
			if (val.empty()) continue;
			if (arrows) { sArrows(vr, y, vw, gLabel); gText(val, vr - 6 - vw, y, gLabel); }
			else gText(val, vr - vw, y, on ? gLabel : gDim);
		}

		// the other side: the picture in a hairline frame (fitted, aspect kept), its caption and a short list
		const float fx = cut + 4, fy = top, fw = px + pw2 - 8 - fx, fh = 72;
		gBox(fx - 1, fy - 1, fw + 2, fh + 2, gLine);
		if (!v.picture.empty())
		{
			struct stat st;
			std::string key = "pause:" + v.picture;
			if (stat(v.picture.c_str(), &st) == 0) key += ":" + std::to_string((long long)st.st_mtime);
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
				float k = std::min(fw * u / iw, fh * u / ih);
				float dw = std::round(iw * k), dh = std::round(ih * k);
				be->draw(it->second.tex, std::round(fx * u + (fw * u - dw) / 2), std::round(fy * u + (fh * u - dh) / 2), dw, dh, Color(1, 1, 1, 1));
			}
		}
		float y = fy + fh + 6;
		if (!v.caption.empty()) { sWrap(v.caption, fx, y, fw, 2, gInk); y += 28; }
		for (size_t i = 0; i < v.lines.size() && i < 7; i++, y += 12) sText(sFit(v.lines[i], fw), fx, y, gDim);

		const float by = py + ph - 18;
		sFill(px + 6, by - 4, pw2 - 12, 1, gLine);
		gChip("A", tr("Choose"), px + 8, by);
		const bool change2 = v.sel >= 0 && v.sel < n && v.rows[v.sel].arrows;
		if (change2) gChip("<>", tr("Change"), px + 14 + gChipWidth("A", tr("Choose")), by);
		gChip("B", tr("Resume"), px + pw2 - 8 - gChipWidth("B", tr("Resume")), by);
	}

	// ---- over any screen: the volume/brightness pill, the battery's notices and the toasts ------------------------
	void Settings::levelBlast(const std::string& label, int percent)
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
		sFill(x0, y0, w, h, VOID);
		gBox(x0, y0, w, h, gMag);
		const char* const* ic = sun ? SUN : mute ? MUTE : VOL;
		for (int r = 0; r < 12; r++)
			for (int c = 0; c < 12; c++)
				if (ic[r][c] == '#') sFill(x0 + 7 + c, y0 + 5 + r, 1, 1, mute ? gRed : sun ? gLabel : gCyan);
		for (int k = 0; k < 10; k++) sFill(x0 + 25 + k * 5, y0 + 6, 4, 10, k < (percent + 5) / 10 ? gLabel : gLine);
		std::string t = std::to_string(percent) + "%";
		sText(t, x0 + w - 7 - sText(t, 0, 0, gLabel, true), y0 + 5, gInk);
	}

	void Settings::noticeBlast(const std::string& title, const std::string& body, int percent, bool charging)
	{
		const float x0 = 14, y0 = 196, w = 282, h = 48;
		rememberCard(x0 * u / s, y0 * u / s, w * u / s, h * u / s);
		sFill(x0, y0, w, h, VOID);
		gBox(x0, y0, w, h, gMag, 2);
		// a battery drawn on the grid: a white case, the charge in cells (red at 10% and under, cyan while it charges)
		const bool low = percent >= 0 && percent <= 10 && !charging;
		const float bx = x0 + 10, by = y0 + 17;
		gBox(bx, by, 26, 14, gInk);
		sFill(bx + 26, by + 4, 2, 6, gInk);
		if (percent >= 0)
		{
			const int cells = std::max(percent > 0 ? 1 : 0, std::min(5, (percent * 5 + 50) / 100));
			for (int k = 0; k < cells; k++) sFill(bx + 2 + k * 5, by + 2, 4, 10, charging ? gCyan : low ? gRed : gLabel);
		}
		if (charging)
		{
			// a bolt over the cells, outlined in black so it reads on them
			static const char* const BOLT[10] = { "....##", "...##.", "..##..", ".#####", "#####.", "..##..", ".##...", ".#....", "#.....", "......" };
			for (int r = 0; r < 10; r++)
				for (int c = 0; c < 6; c++)
					if (BOLT[r][c] == '#') sFill(bx + 10 + c, by + 2 + r, 1, 1, gLabel);
		}
		const float tx = x0 + 46, tw = w - 46 - 8;
		gText(sFit(up(tr(title)), tw), tx, y0 + 8, low ? gRed : gLabel);
		sWrap(tr(body), tx, y0 + 22, tw, 2, gInk);
	}

	// the toast across the top: the badge (or the glyph's sign on the accent) in a hairline frame, cut to 24 x 24 by
	// averaging and snapped to 9-bit colour so it sits on the grid; the title in yellow and the line under it in white
	void Settings::toastBlast(const char* const* sign)
	{
		const float x0 = 5, y0 = 4, w = 300, h = 34;
		rememberCard(x0 * u / s, y0 * u / s, w * u / s, h * u / s);
		sFill(x0, y0, w, h, VOID);
		gBox(x0, y0, w, h, gCyan);
		const float bx = x0 + 6, by = y0 + 5;
		gBox(bx - 1, by - 1, 26, 26, gLine);
		rnds::TexPtr badge;
		if (!toastNow.badge.empty())
		{
			std::string key = "gbadge:" + toastNow.badge;
			auto it = cache.find(key);
			if (it == cache.end())
			{
				rnds::Bitmap b = rnds::decodeFile(toastNow.badge), o;
				Cached c;
				if (!b.empty() && !b.alpha)
				{
					const int n = 24;
					o.w = o.h = n;
					o.px.assign(n * n * 4, 0);
					for (int y = 0; y < n; y++)
						for (int x = 0; x < n; x++)
						{
							int ya = y * b.h / n, yb = std::max(ya + 1, (y + 1) * b.h / n);
							int xa = x * b.w / n, xb = std::max(xa + 1, (x + 1) * b.w / n);
							double r = 0, g = 0, bl = 0, a = 0;
							int cnt = 0;
							for (int yy = ya; yy < yb; yy++)
								for (int xx = xa; xx < xb; xx++)
								{
									const uint8_t* q = &b.px[(yy * b.w + xx) * 4];
									double qa = q[3] / 255.0;
									r += q[0] * qa; g += q[1] * qa; bl += q[2] * qa; a += qa; cnt++;
								}
							uint8_t* d = &o.px[(y * n + x) * 4];
							if (a <= 0) continue;
							auto q3 = [](double v) { int k = (int)std::lround(std::min(255.0, v) / 255.0 * 7); return (uint8_t)((k * 255 + 3) / 7); };
							d[0] = q3(r / a); d[1] = q3(g / a); d[2] = q3(bl / a);
							d[3] = a / cnt >= 0.5 ? 255 : 0;
						}
					c.tex = be->upload(o, false);
				}
				it = cache.emplace(key, c).first;
			}
			badge = it->second.tex;
		}
		const Color acc = Color::hex(toastNow.accent);
		sFill(bx, by, 24, 24, acc);
		if (badge)
			be->draw(badge, bx * u, by * u, 24 * u, 24 * u, Color(1, 1, 1, 1));
		else
			for (int r = 0; r < 9; r++)
				for (int c = 0; c < 9; c++)
					if (sign[r][c] == '#') sFill(bx + 3 + c * 2, by + 3 + r * 2, 2, 2, VOID);
		const float tx = bx + 31, tw = x0 + w - 7 - tx;
		gText(sFit(toastNow.l1, tw), tx, y0 + 4, gLabel);
		sText(sFit(toastNow.l2, tw), tx, y0 + 18, gInk);
	}
}
