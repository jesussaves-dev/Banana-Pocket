// banana-ui --headless: the scripted test runner, drawing to PNGs with no panel.
#include "app.h"
#include "../third_party/stb_image_write.h"

// ---- headless: the same UI rendered to PNGs, for tests and screenshots ------------------------------------------
// the library scan's time (set by main before runHeadless, which the `boot` command reports)
int runHeadless(const Options& o, Host& lib, const std::string& variant, const std::string& theme, double scanMs)
{
	SoftBackend be(o.w, o.h);
	std::string layout = themePath(theme);
	// a 640-wide layout, scaled to the panel
	float sc = o.w / 640.0f;
	float canvasW = o.w / sc, canvasH = o.h / sc;
	double tMenu = nowMs();	// the menu's start-up: its constructor (the theme's build) below, then the first settle
	if (!isLayoutTheme(layout)) layout = themePath("user:pocket");
	std::unique_ptr<Menu> uiPtr(new LayoutMenu(&lib, &be, layout, sc, o.assets + "/fonts", o.banana + "/fonts", canvasW, canvasH));
	Menu& ui = *uiPtr;
	double now = 100000;
	std::stringstream ss(o.script);
	std::string cmd;
	// the menu's start-up, in three parts, at this canvas size (the scan ran before runHeadless).
	// `boot` prints them as one line; see docs/PERFORMANCE.md.
	double bootScan = scanMs, bootMenu = 0, bootFrame = 0;
	auto settle = [&] { ui.update(now); ui.finishWork(); ui.update(now); };
	auto save = [&](const std::string& path)
	{
		std::vector<uint8_t> rgb((size_t)o.w * o.h * 3);
		const uint32_t* p = be.pixels();
		for (size_t i = 0; i < (size_t)o.w * o.h; i++) { rgb[i * 3] = (p[i] >> 16) & 255; rgb[i * 3 + 1] = (p[i] >> 8) & 255; rgb[i * 3 + 2] = p[i] & 255; }
		return stbi_write_png(path.c_str(), o.w, o.h, 3, rgb.data(), o.w * 3) != 0;
	};
	ui.setHome(0);
	// Settings builds its list in the constructor (which runs here), so the theme variant is set just after it
	Settings settings(&be, o.assets, o.data, sc, variant, &lib);
	settings.setPanelHeight(canvasH);
	settings.setSync(true);
	settings.searchCount = [&](const std::string& q) { return lib.countMatches(q); };
	settings.setOverride = [&](const std::string& s, const std::string& r, const std::string& c, const std::string& sh, const std::string& f)
		{ lib.setOverride(s, r, c, sh, f); };
	settings.setFrameSystems(lib.frameSystems());
	LowBattery low;
	IdleGuard guard;	// the burn-in guard's logic, driven by the `guard`/`idle` script commands (test only)
	guard.configure(atoi(getConfValue(o.data + "/bananad.conf", "dim_minutes", "2").c_str()),
		atoi(getConfValue(o.data + "/bananad.conf", "dim_percent", "30").c_str()));
	// the overlay runs in endScreen, over whatever screen the menu drew: the battery's notice, the volume's level,
	// and the achievement toasts. None of them is gated on a page being open -- what a game unlocked is for the screen
	// the player comes back to, and the battery is the battery.
	be.overlay = [&]()
	{
		// endScreen runs inside the locked update, where the loop's own `now` is not in scope; the settings-view
		// overlay took its own time for the same reason
		double t = nowMs();
		if (low.showing(t)) settings.notice(low.title, low.body, low.percent);
		else settings.drawToast(t);
	};
	// the menu's start-up, timed before any script command: its constructor (the theme's build) ran above, the first
	// update lays out the home shelf, finishWork decodes its pictures, the first render draws the first frame
	{
		ui.update(now); ui.finishWork(); ui.update(now);
		bootMenu = nowMs() - tMenu;
		double f0 = nowMs();
		ui.render();
		bootFrame = nowMs() - f0;
	}
	while (std::getline(ss, cmd, ';'))
	{
		std::stringstream cs(cmd);
		std::string op;
		cs >> op;
		ui.update(now);
		if (op == "home") { int s = 0; cs >> s; ui.setHome(s); }
		else if (op == "lib") { int s = 0, g = 0; cs >> s >> g; lib.openLibrary(s); ui.setLibrary(s, g); }
		else if (op == "wait") { double ms = 0; cs >> ms; now += ms; }
		else if (op == "guard")
		{
			// guard <brightness file> <max file>: attach the burn-in guard to files instead of /sys (test only)
			std::string bl, mx; cs >> bl >> mx;
			guard.attach(bl, mx);
			guard.input(now);
			logf("guard: attached %s", bl.c_str());
		}
		else if (op == "idle")
		{
			// idle <ms>: let that much idle time pass and apply the guard's step
			double ms = 0; cs >> ms;
			now += ms;
			bool ch = guard.tick(now, nullptr);
			logf("guard: idle +%.0f ms -> stage %d, level %d%s", ms, guard.stage(), guard.level(), ch ? " (changed)" : "");
		}
		else if (op == "battery")
		{
			// battery <percent> [charging]: what /sys would say from now on
			std::string ch;
			cs >> lib.testPower.percent >> ch;
			lib.testPower.charging = ch == "charging";
			if (low.check(lib.power(), shutdownPercent(o.data), now)) logf("battery %d%%: notice %s", lib.testPower.percent, low.showing(now) ? low.title.c_str() : "hidden");
		}
		else if (op == "resume")
		{
			// resume [take|cancel]: the boot decision, for the headless checks (test only)
			std::string what, rom;
			cs >> what;
			Resume p = resumePlan(o.data, rom);
			const char* name = p == Resume::Ready ? "ready" : p == Resume::Missing ? "missing" : p == Resume::Blocked ? "blocked" : "none";
			logf("resume: %s%s%s", name, p == Resume::Ready ? " " : "", p == Resume::Ready ? rom.c_str() : "");
			if (p == Resume::Ready && what == "take") resumeTake(o.data);
			else if (what == "cancel") resumeCancel(o.data);
			else if (p == Resume::Missing || p == Resume::Blocked) resumeCancel(o.data);
		}
		else if (op == "wifi-add")
		{
			// wifi-add <ssid> <key> <wep|wpa>: remember a network in the card's wifi.conf (test only)
			std::string ssid, key, kind;
			cs >> ssid >> key >> kind;
			wifi::Network n;
			n.ssid = ssid; n.secured = true; n.wep = kind == "wep";
			bool ok = wifi::remember(o.data + "/wifi.conf", n, key);
			logf("wifi-add %s %s: %s", ssid.c_str(), kind.c_str(), ok ? "ok" : "no");
		}
		else if (op == "wifi-scan")
		{
			// wifi-scan <bss-flags>: what parseScan makes of one scan line (test only)
			std::string flags;
			cs >> flags;
			std::string line = "00:11:22:33:44:55\t2412\t-50\t" + flags + "\tTestNet\n";
			auto nets = wifi::parseScan(line);
			logf("wifi-scan %s: secured=%d wep=%d unsupported=%d", flags.c_str(),
				nets.empty() ? 0 : (int)nets[0].secured, nets.empty() ? 0 : (int)nets[0].wep,
				nets.empty() ? 0 : (int)nets[0].unsupported);
		}
		else if (op == "wifi")
		{
			// wifi <ssid> <up|down>: move a saved network in the card's wifi.conf (test only; the Network page's L/R)
			std::string ssid, d;
			cs >> ssid >> d;
			bool ok = wifi::bump(o.data + "/wifi.conf", ssid, d == "up" ? -1 : 1);
			logf("wifi %s %s: %s", ssid.c_str(), d.c_str(), ok ? "ok" : "no change");
		}
		else if (op == "toast")
		{
			// toast <glyph>|<title>|<body>|<badge>: queue a pop-up as an achievement (with the badge RetroArch
			// downloads) or as /run/banana/toast would; test only, so the toast is exercised without a game. The
			// fields are pipe-separated (a title holds spaces), `badge` empty for a plain message.
			std::string rest; std::getline(cs >> std::ws, rest);
			std::vector<std::string> f;
			size_t s = 0;
			while (f.size() < 3)
			{
				size_t bar = rest.find('|', s);
				if (bar == std::string::npos) break;
				f.push_back(rest.substr(s, bar - s));
				s = bar + 1;
			}
			f.push_back(rest.substr(s));
			while (f.size() < 4) f.push_back(std::string());
			settings.notify(f[1], f[2], std::max(0, std::min(2, atoi(f[0].c_str()))), f[3]);
			logf("toast: queued \"%s\" (badge %s)", f[1].c_str(), f[3].empty() ? "none" : "yes");
		}
		else if (op == "sysinfo")
		{
			// sysinfo <index>: the system as the shelf sees it (its games, plays and RetroAchievements totals); the
			// theme draws the label, and the test reads it here rather than by reading a rendered string
			int s = 0; cs >> s;
			rnds::SysInfo si = lib.system(s);
			logf("sysinfo %s: %d games, %d played, %s, achievements %d/%d", si.id.c_str(), si.games, si.played,
				si.time.c_str(), si.achA, si.achB);
		}
		else if (op == "apps")
		{
			// apps: the app systems the registry scanned (Library::scanApps), one line each -- the Apps shelf, as the
			// test reads it without a rendered tile
			for (int i = 0; i < lib.systemCount(); i++)
			{
				rnds::SysInfo si = lib.system(i);
				if (si.app) logf("apps %d: %s (%s), %d entries", i, si.id.c_str(), si.name.c_str(), si.games);
			}
		}
		else if (op == "press")
		{
			std::string b; cs >> b;
			// the first press while the burn-in guard has the panel dimmed/off wakes it and is swallowed
			if (guard.wake(now, nullptr)) { logf("press %s: swallowed by the burn-in guard", b.c_str()); continue; }
			guard.input(now);
			static const std::map<std::string, Key> keys = { { "left", Key::LEFT }, { "right", Key::RIGHT }, { "up", Key::UP }, { "down", Key::DOWN },
				{ "a", Key::A }, { "b", Key::B }, { "x", Key::X }, { "y", Key::Y }, { "l", Key::L }, { "r", Key::R }, { "start", Key::START }, { "select", Key::SELECT } };
			Menu::Button btn;
			auto it = keys.find(b);
			if (it == keys.end()) { logf("unknown button %s", b.c_str()); return 2; }
			if (low.hide(now)) logf("press %s: notice hidden", b.c_str());
			if (!settings.isOpen() && it->second == Key::SELECT) { settings.openSearch(lib.searchQuery()); logf("press select: search open"); continue; }
			if (!settings.isOpen() && it->second == Key::X && ui.view() == Menu::LIBRARY)
			{
				// X on a game: its own options (the core and shader it runs with), over the library
				int g = ui.gameIndex();
				const SystemDef& sd = lib.systemOf(g);
				std::string rel = lib.gameAt(g).rel;
				auto ov = lib.overrideOf(sd.id, rel);
				settings.openGame(sd.id, rel, lib.gameAt(g).path, lib.gameAt(g).title, ov.core, ov.shader, ov.frame, lib.framable(g));
				logf("press x: game options open (%s/%s)", sd.id.c_str(), rel.c_str());
				continue;
			}
			if (settings.isOpen() || it->second == Key::START)
			{
				if (!settings.isOpen()) { settings.open(); logf("press %s: settings open", b.c_str()); continue; }
				int r = (int)settings.press(it->second);
				if (r == Settings::SEARCH)
				{
					lib.setSearch(settings.searchText);
					ui.dataChanged();
					int sh = lib.searchShelf();
					if (sh >= 0) { lib.openLibrary(sh); ui.setLibrary(sh, 0); }
					else ui.setHome(0);
					logf("search \"%s\": shelf %d, %d games", settings.searchText.c_str(), sh, sh >= 0 ? (int)lib.shelf(sh).games.size() : -1);
				}
				logf("press %s: settings %s", b.c_str(), settings.isOpen() ? ("result " + std::to_string(r)).c_str() : "closed");
				continue;
			}
			if (!uiButton(it->second, btn)) { logf("unknown button %s", b.c_str()); return 2; }
			bool used = letterJump(ui, lib, btn) || ui.press(btn);
			logf("press %s: %s, view %d sys %d game %d", b.c_str(), used ? "used" : "unused", (int)ui.view(), ui.systemIndex(), ui.gameIndex());
		}
		else if (op == "setup") { settings.setup(); logf("setup open"); }
		// search <text>: the search keyboard open with that text typed (START then searches)
		else if (op == "search") { std::string t; std::getline(cs, t); size_t b = t.find_first_not_of(' '); settings.openSearch(b == std::string::npos ? "" : t.substr(b)); logf("search open: \"%s\"", t.c_str()); }
		else if (op == "mem")
		{
			// mem: what the process holds now (RSS) and the texture cache's share of it
			std::ifstream st("/proc/self/status");
			std::string ln, rss;
			while (std::getline(st, ln)) if (ln.compare(0, 6, "VmRSS:") == 0) rss = ln.substr(6);
			logf("mem: RSS%s", rss.c_str());
		}
		else if (op == "touch")
		{
			// touch <x> <y>: a tap, in layout px
			float x = 0, y = 0;
			cs >> x >> y;
			if (guard.wake(now, nullptr)) { logf("touch %.0f %.0f: swallowed by the burn-in guard", x, y); continue; }
			guard.input(now);
			float k = o.w / 640.0f;
			if (settings.isOpen())
			{
				settings.touch(x * k, y * k, true);
				logf("touch %.0f %.0f: settings result %d", x, y, (int)settings.touch(x * k, y * k, false));
			}
			else
			{
				ui.touch(x * k, y * k, true);
				logf("touch %.0f %.0f: %s", x, y, ui.touch(x * k, y * k, false) ? "used" : "unused");
			}
		}
		else if (op == "shot")
		{
			std::string name; cs >> name;
			double t0 = 0, t1 = 0;
			if (settings.isOpen())
			{
				settings.update(now);	// what came back since the last command (a press skips the update below)
				settings.update(now);
				settings.render();
				t0 = nowMs(); settings.render(); t1 = nowMs();
			}
			else
			{
				settle(); ui.render(); settle(); ui.render();
				t0 = nowMs();
				ui.render();
				t1 = nowMs();
			}
			if (!save(o.headless + name + ".png")) { logf("can't write %s", name.c_str()); return 1; }
			logf("shot %s (frame %.2f ms, wake %d)", name.c_str(), t1 - t0, ui.nextWake());
		}
		else if (op == "splash")
		{
			// splash <name> <ms> [ready]: the boot animation at that time
			std::string name; double t = 0; int ready = 0;
			cs >> name >> t >> ready;
			static Splash sp;
			static bool loaded = false;
			if (!loaded && !(loaded = sp.load(o.splash, o.banana + "/banana-48.png", variant, o.w, o.h))) { logf("no splash assets in %s", o.splash.c_str()); return 1; }
			double s0 = nowMs();
			sp.draw(be.canvas(), t, ready != 0);
			double s1 = nowMs();
			if (!save(o.headless + name + ".png")) return 1;
			logf("splash %s at %.0f ms (frame %.2f ms)", name.c_str(), t, s1 - s0);
		}
		else if (op == "boot")
		{
			logf("boot-metrics: scan %.1f ms, menu %.1f ms, first-frame %.1f ms (canvas %dx%d)", bootScan, bootMenu, bootFrame, o.w, o.h);
		}
		else if (op == "bench")
		{
			// bench N [EVERY BUTTON]: N frames 16.7 ms apart, as the panels would show them: the main thread's time per
			// frame (the engine's and the backend's; the drawing alone after it, then the picture worker's, done between
			// the two here), pressing
			// BUTTON every EVERY frames (scrolling)
			int n = 60, every = 0; std::string bn;
			cs >> n >> every >> bn;
			Key bk = bn == "left" ? Key::LEFT : bn == "up" ? Key::UP : bn == "down" ? Key::DOWN : Key::RIGHT;
			double total = 0, worst = 0, draw = 0, drawWorst = 0, work = 0, workWorst = 0;
			be.skipped = 0;
			be.redrawn = 0;
			for (int i = 0; i < n; i++)
			{
				now += 1000.0 / 60;
				Menu::Button btn;
				if (every > 0 && i % every == 0 && uiButton(bk, btn)) ui.press(btn);
				double t0 = nowMs();
				ui.update(now);
				double t1 = nowMs();
				ui.finishWork();
				double t2 = nowMs();
				ui.render();
				double t3 = nowMs(), dt = (t1 - t0) + (t3 - t2);
				total += dt; worst = std::max(worst, dt);
				draw += t3 - t2; drawWorst = std::max(drawWorst, t3 - t2);
				work += t2 - t1; workWorst = std::max(workWorst, t2 - t1);
			}
			// (the main thread's time: the worker's runs beside it on the device)
			logf("bench: %d frames, %.2f ms average, %.2f ms worst (drawing %.2f / %.2f; the worker %.2f / %.2f), %d of %d screens unchanged, %.1f%% of pixels drawn again", n, total / n, worst,
				draw / n, drawWorst, work / n, workWorst, be.skipped, 2 * n, 100.0 * be.redrawn / (2.0 * n * be.width() * be.height()));
		}
		else if (!op.empty()) { logf("unknown command %s", op.c_str()); return 2; }
		if (settings.isOpen()) { settings.update(now); settings.update(now); }
		if (lib.pendingLaunch() >= 0)
		{
			int g = lib.pendingLaunch();
			logf("launch %d: %s (system %s)", g, lib.gameAt(g).path.c_str(), lib.systemOf(g).id.c_str());
			lib.clearLaunch();
		}
		ui.update(now);
		if (rearrange(lib, ui)) logf("shelf: %d systems, cursor on %s", lib.systemCount(), lib.shelf(ui.systemIndex()).def.id.c_str());
	}
	return 0;
}

