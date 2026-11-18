// banana-ui: the pause menu over a frozen game.
#include "app.h"
#include "pause_menu.h"
#include <cctype>
#include <cstdlib>
#include <linux/input.h>

void PauseMenu::build()
{
	Settings::PauseView& v = view;
	v.sel = sel;
	v.armed = armed;
	v.title = gameIndex >= 0 ? lib.gameAt(gameIndex).title : std::string();
	std::string status;
	int pct = lib.power().percent;
	if (pct >= 0) status = std::to_string(pct) + "%   ";
	status += lib.clockText();
	long mins = (long)((nowMs() - gameStart) / 60000);
	char played[48];
	snprintf(played, sizeof played, mins >= 60 ? "   %ldh%02ld" : "   %ldm", mins >= 60 ? mins / 60 : mins, mins % 60);
	v.status = status + played;
	int prog = -1, total = 0;
	if (FILE* f = fopen("/run/banana/ra-progress", "r")) { if (fscanf(f, "%d %d", &prog, &total) != 2) prog = -1; fclose(f); }
	v.rows.clear();
	for (const auto& id : rows)
	{
		Settings::PauseRow r;
		r.label = id;
		if (gameRA && (id == "Save state" || id == "Load state")) { r.value = tr("Slot") + " " + std::to_string(pickSlot); r.arrows = true; }
		else if (id == "Change disc") { r.value = tr("Disc") + " " + std::to_string(discPick) + "/" + std::to_string(discs); r.arrows = true; }
		else if (id == "Fast-forward") r.value = ffOn ? "On" : "Off";
		else if (id == "Frame") { r.value = frameKind == FrameKind::Art ? "Console" : frameKind == FrameKind::Banana ? "Banana" : "None"; r.arrows = true; }
		else if (id == "Achievements" && prog >= 0) r.value = std::to_string(prog) + "/" + std::to_string(total);
		v.rows.push_back(r);
	}
	const std::string row = sel >= 0 && sel < (int)rows.size() ? rows[sel] : std::string();
	v.picture.clear();
	v.caption.clear();
	v.lines.clear();
	if (gameRA && (row == "Save state" || row == "Load state"))
	{
		std::string st = statePath(gameRom, pickSlot);
		std::string slot = tr("Slot") + " " + std::to_string(pickSlot);
		struct stat sb;
		if (!st.empty() && stat(st.c_str(), &sb) == 0)
		{
			if (access((st + ".png").c_str(), R_OK) == 0) v.picture = st + ".png";
			char when[32];
			struct tm tmv;
			localtime_r(&sb.st_mtime, &tmv);
			strftime(when, sizeof when, lib.twelveHour ? "%Y-%m-%d %I:%M %p" : "%Y-%m-%d %H:%M", &tmv);
			v.caption = slot + "   " + when;
		}
		else v.caption = slot + "   " + tr("Empty");
	}
	else if (row == "Frame") v.caption = tr("Saved for this game");
	else if (row == "Change disc")
		v.caption = tr("Disc") + " " + std::to_string(discNow) + "/" + std::to_string(discs) +
		            (discPick != discNow ? "   ->   " + std::to_string(discPick) : std::string());
	else if (row == "Achievements")
	{
		if (prog >= 0) v.caption = std::to_string(prog) + " / " + std::to_string(total);
		// this session's unlocks, newest first: cheevos is tab-separated (unlocked, total, title, description,
		// points, rarity, badge); the newest one's badge is the picture
		std::vector<std::string> got;
		std::ifstream f("/run/banana/cheevos");
		for (std::string line; std::getline(f, line);)
		{
			std::vector<std::string> fld;
			for (size_t a = 0, b; ; a = b + 1)
			{
				b = line.find('\t', a);
				fld.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
				if (b == std::string::npos) break;
			}
			if (fld.size() < 3 || fld[2].empty()) continue;
			got.insert(got.begin(), fld[2]);
			if (fld.size() >= 7 && !fld[6].empty() && access(fld[6].c_str(), R_OK) == 0) v.picture = fld[6];
		}
		v.lines = got;
	}
	if (v.picture.empty() && gameIndex >= 0) v.picture = lib.gameAt(gameIndex).box;
}
void PauseMenu::draw() { build(); drawCard(); }

// "F2", "SPACE", "R" ... -> an evdev key code (0: unknown)
static int keyByName(std::string n)
{
	for (char& c : n) c = (char)toupper((unsigned char)c);
	if (n.size() >= 2 && n[0] == 'F' && isdigit((unsigned char)n[1]))
	{
		int f = atoi(n.c_str() + 1);
		if (f >= 1 && f <= 10) return KEY_F1 + f - 1;
		if (f == 11) return KEY_F11;
		if (f == 12) return KEY_F12;
		return 0;
	}
	if (n == "SPACE") return KEY_SPACE;
	if (n == "ENTER") return KEY_ENTER;
	if (n == "TAB") return KEY_TAB;
	if (n == "ESC") return KEY_ESC;
	if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z')
	{
		static const int az[26] = { KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
		                            KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
		return az[n[0] - 'A'];
	}
	return 0;
}
// "Save state=F2,Load state=F4" -> the app's rows that tap a key
static std::vector<std::pair<std::string, int>> parseMenuKeys(const std::string& spec)
{
	std::vector<std::pair<std::string, int>> out;
	size_t at = 0;
	while (at < spec.size())
	{
		size_t end = spec.find(',', at);
		if (end == std::string::npos) end = spec.size();
		std::string item = spec.substr(at, end - at);
		at = end + 1;
		size_t eq = item.find('=');
		if (eq == std::string::npos) continue;
		std::string label = item.substr(0, eq);
		while (!label.empty() && label.front() == ' ') label.erase(label.begin());
		while (!label.empty() && label.back() == ' ') label.pop_back();
		int code = keyByName(item.substr(eq + 1));
		if (!label.empty() && code) out.push_back({ label, code });
	}
	return out;
}

// The pause menu: MENU over a game freezes it (SIGSTOP on the launcher's whole tree, so a port or BananaTube stops
// too) and lists what can be done; RetroArch's own menu stays one row away (and on MENU + X). It replaces the
// RetroArch menu as the first thing MENU opens: fewer choices, Banana's look, the same for every game.
void PauseMenu::show()
{
	if (gamePid <= 0) { pending = false; return; }
	bool ok = input.grab(true);
	open = true;
	pending = false;
	sel = 0;
	armed = -1;
	pickSlot = raSlot;
	discPick = discNow;
	if (gameRA)
	{
		rows = { "Resume", "Save state", "Load state" };
		if (discs > 1) rows.push_back("Change disc");
		rows.push_back("Fast-forward");
		rows.push_back("Shader");
		if (gameFramable && frameMaster()) rows.push_back("Frame");
		if (access("/run/banana/ra-progress", R_OK) == 0) rows.push_back("Achievements");
		for (const char* r : { "Restart game", "RetroArch menu", "Quit game" }) rows.push_back(r);
	}
	else
	{
		appKeys.clear();
		if (gameIndex >= 0) appKeys = parseMenuKeys(lib.systemOf(gameIndex).menuKeys);
		rows = { "Resume" };
		for (const auto& k : appKeys) rows.push_back(k.first);
		if (gameFramable && frameMaster()) rows.push_back("Frame");
		rows.push_back("Quit");
	}
	signalTree(gamePid, SIGSTOP);
	if (FILE* f = fopen("/run/banana/overlay-open", "w")) { fputs("1\n", f); fclose(f); }
	draw();
	logf("quick menu: open, game frozen%s", ok ? "" : " (EVIOCGRAB failed)");
}
void PauseMenu::close()
{
	if (!open && !pending) return;
	bool was = open;
	open = pending = false;
	if (!was) return;
	signalTree(gamePid, SIGCONT);
	input.grab(false);
	unlink("/run/banana/overlay-open");
	hideCard();
	comp.reconfigure();	// a Frame row change: the game hears its new size now that it can answer
	logf("quick menu: closed, game running");
}
// RetroArch has no "save to slot N": step its slot to the one picked here first (queued: sent once it is thawed)
void PauseMenu::syncSlot()
{
	for (; raSlot < pickSlot; raSlot++) raQueue.push_back("STATE_SLOT_PLUS");
	for (; raSlot > pickSlot; raSlot--) raQueue.push_back("STATE_SLOT_MINUS");
}
// A on a row. Load state, Restart and Quit lose progress, so they ask "Press A again" first.
void PauseMenu::act()
{
	const std::string row = sel >= 0 && sel < (int)rows.size() ? rows[sel] : std::string();
	bool risky = row == "Load state" || row == "Restart game" || row == "Quit game" || row == "Quit";
	if (risky && armed != sel) { armed = sel; draw(); return; }
	logf("quick menu: %s", row.c_str());
	if (row == "Achievements" || row == "Frame" || (row == "Change disc" && discPick == discNow)) return;	// nothing to do
	if (row == "Fast-forward")
	{
		// stays open: the line is queued and goes out on Resume; the row shows the new state
		raQueue.push_back("FAST_FORWARD");
		ffOn = !ffOn;
		draw();
		return;
	}
	for (const auto& k : appKeys)
		if (!gameRA && k.first == row) { close(); keyQueue.push_back(k.second); return; }	// tapped by the main loop once the client has the keys again
	pid_t game = gamePid;
	close();	// thawed first: RetroArch reads its stdin only while it runs
	if (row == "Save state") { syncSlot(); raQueue.push_back("SAVE_STATE"); }
	else if (row == "Load state") { syncSlot(); raQueue.push_back("LOAD_STATE"); }
	else if (row == "Change disc")
	{
		// the tray opens, the disc index moves, the tray closes: RetroArch's own disc-change sequence
		raQueue.push_back("DISK_EJECT_TOGGLE");
		for (; discNow < discPick; discNow++) raQueue.push_back("DISK_NEXT");
		for (; discNow > discPick; discNow--) raQueue.push_back("DISK_PREV");
		raQueue.push_back("DISK_EJECT_TOGGLE");
	}
	else if (row == "Shader") raQueue.push_back("SHADER_TOGGLE");
	else if (row == "Restart game") raQueue.push_back("RESET");
	else if (row == "RetroArch menu") raQueue.push_back("MENU_TOGGLE");
	else if (row == "Quit game") raQueue.push_back("QUIT");
	else if (row == "Quit" && game > 0) kill(game, SIGTERM);	// the launcher's trap ends the app's own session
}
void PauseMenu::key(Key k)
{
	int n = (int)rows.size();
	const std::string row = sel >= 0 && sel < n ? rows[sel] : std::string();
	if (k == Key::B || k == Key::MENU) close();
	else if (k == Key::A) act();
	else if ((k == Key::UP || k == Key::DOWN) && n > 0)
	{
		sel = (sel + (k == Key::UP ? n - 1 : 1)) % n;
		armed = -1;
		draw();
	}
	else if (k == Key::LEFT || k == Key::RIGHT)
	{
		int d = k == Key::LEFT ? -1 : 1;
		if (gameRA && (row == "Save state" || row == "Load state")) pickSlot = (pickSlot + d + 10) % 10;
		else if (row == "Change disc") discPick = (discPick - 1 + d + discs) % discs + 1;
		else if (row == "Frame" && gameIndex >= 0)
		{
			const SystemDef& sd = lib.systemOf(gameIndex);
			std::vector<FrameKind> kinds = { FrameKind::None };
			if (frameHasArt(sd.id)) kinds.push_back(FrameKind::Art);
			kinds.push_back(FrameKind::Banana);
			int kindCount = (int)kinds.size(), at = (int)(std::find(kinds.begin(), kinds.end(), frameKind) - kinds.begin());
			FrameKind want = kinds[(at + d + kindCount) % kindCount];
			frameKind = applyFrame(sd.id, want);
			if (frameKind == want)
			{
				const std::string rel = lib.gameAt(gameIndex).rel;
				GameOverride ov = lib.overrideOf(sd.id, rel);
				lib.setOverride(sd.id, rel, ov.core, ov.shader, frameKindWord(want));
			}
		}
		else return;
		armed = -1;
		draw();
	}
}
