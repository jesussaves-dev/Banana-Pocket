#pragma once
// banana-ui: what the main loop, the headless runner and the boot animation share.
#include "frontend.h"
#include "input.h"
#include "kms.h"
#include "util.h"
#include "compositor.h"
#include "library.h"
#include "frame.h"
#include "soft.h"
#include "splash.h"
#include "settings.h"
#include "i18n.h"
#include "settings_defaults.h"
#include "theme.h"
#include "sound.h"
#include "idle.h"
#include "menu.h"
#include "layout.h"
#include <algorithm>
#include <deque>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <iterator>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <list>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace banana;

static double nowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// usleep is obsolete in POSIX.1-2008; nanosleep is its replacement
static void sleepUs(long us) { timespec ts = { us / 1000000, (us % 1000000) * 1000 }; while (nanosleep(&ts, &ts) && errno == EINTR) {} }

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "banana-ui: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

// ---- the pause menu's reach into a game ---------------------------------------------------------------------
// A signal to a process and every descendant of it. The launcher is its own process group (runGame's setsid), but
// run-port and run-bananatube start the app with setsid again, so a group signal would miss the app itself: the
// tree is walked through /proc instead. Used to freeze a game (SIGSTOP) under the pause menu and thaw it (SIGCONT).
static void signalTree(pid_t root, int sig)
{
	if (root <= 0) return;
	std::vector<std::pair<pid_t, pid_t>> procs;	// (pid, ppid)
	if (DIR* d = opendir("/proc"))
	{
		while (dirent* e = readdir(d))
		{
			char* end = nullptr;
			long pid = strtol(e->d_name, &end, 10);
			if (pid <= 0 || *end) continue;
			char path[64], buf[512];
			snprintf(path, sizeof path, "/proc/%ld/stat", pid);
			FILE* f = fopen(path, "r");
			if (!f) continue;
			size_t n = fread(buf, 1, sizeof buf - 1, f);
			fclose(f);
			buf[n] = 0;
			const char* rp = strrchr(buf, ')');	// the command name can hold spaces and parentheses
			int ppid = 0;
			if (rp && sscanf(rp + 1, " %*c %d", &ppid) == 1) procs.push_back({ (pid_t)pid, (pid_t)ppid });
		}
		closedir(d);
	}
	std::vector<pid_t> tree{ root };
	for (size_t i = 0; i < tree.size(); i++)
		for (const auto& pr : procs)
			if (pr.second == tree[i] && std::find(tree.begin(), tree.end(), pr.first) == tree.end()) tree.push_back(pr.first);
	for (pid_t pid : tree) kill(pid, sig);
}

// One command line to RetroArch through its stdin FIFO (run-retroarch holds the pipe open; stdin_cmd_enable). Never
// blocks: no reader means no RetroArch, and the line is dropped.
static bool retroarchCmd(const char* cmd)
{
	int fd = open("/run/banana/retroarch.cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return false;
	std::string line = std::string(cmd) + "\n";
	ssize_t w;
	do w = write(fd, line.data(), line.size()); while (w < 0 && errno == EINTR);
	close(fd);
	return w == (ssize_t)line.size();
}

// A save state of the running game: "<rom stem>.state" for slot 0, ".stateN" for N, in /data/states or one of its
// per-core folders (RetroArch sorts them by core). The newest match wins; "" when the slot has none.
static std::string statePath(const std::string& rom, int slot)
{
	std::string base = rom.substr(rom.rfind('/') + 1);
	size_t dot = base.rfind('.');
	if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
	std::string name = base + ".state" + (slot > 0 ? std::to_string(slot) : std::string());
	std::vector<std::string> dirs = { "/data/states" };
	if (DIR* d = opendir("/data/states"))
	{
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.') dirs.push_back(std::string("/data/states/") + e->d_name);
		closedir(d);
	}
	std::string best;
	time_t bestAt = 0;
	for (const auto& dir : dirs)
	{
		struct stat st;
		std::string pth = dir + "/" + name;
		if (stat(pth.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (best.empty() || st.st_mtime > bestAt)) { best = pth; bestAt = st.st_mtime; }
	}
	return best;
}

// How many discs a game has: the entries of an .m3u playlist (RetroArch's multi-disc format), else 1.
static int discCount(const std::string& rom)
{
	size_t dot = rom.rfind('.');
	std::string ext = dot == std::string::npos ? "" : rom.substr(dot + 1);
	for (auto& c : ext) c = (char)tolower((unsigned char)c);
	if (ext != "m3u") return 1;
	std::ifstream f(rom);
	std::string line;
	int n = 0;
	while (std::getline(f, line))
	{
		size_t a = line.find_first_not_of(" \t\r");
		if (a != std::string::npos && line[a] != '#') n++;
	}
	return std::max(1, n);
}

// ---- resume the game a shutdown interrupted ---------------------------------------------------------------
// /data/banana/resume is one line, the ROM's absolute path, written by bananad when a shutdown interrupts a game;
// /data/banana/resume-tries bounds how many boots may try it before the menu gives up (a game that wedges the
// device must not loop the boot). These three are the whole policy, so the headless harness can check it.
enum class Resume { None, Ready, Missing, Blocked };

static Resume resumePlan(const std::string& data, std::string& rom)
{
	int tries = 0;
	if (std::ifstream f(data + "/resume-tries"); f) f >> tries;
	if (tries >= 3) return Resume::Blocked;
	std::ifstream f(data + "/resume");
	if (!f) return Resume::None;
	std::getline(f, rom);
	while (!rom.empty() && (rom.back() == '\r' || rom.back() == '\n' || rom.back() == ' ')) rom.pop_back();
	if (rom.empty()) return Resume::Missing;
	return access(rom.c_str(), R_OK) == 0 ? Resume::Ready : Resume::Missing;
}
static void resumeTake(const std::string& data)	// it launched: count it and consume the marker
{
	int tries = 0;
	if (std::ifstream f(data + "/resume-tries"); f) f >> tries;
	std::ofstream(data + "/resume-tries") << (tries + 1) << "\n";
	unlink((data + "/resume").c_str());
}
static void resumeCancel(const std::string& data)	// cancelled, missing or exhausted: forget it all
{
	unlink((data + "/resume").c_str());
	unlink((data + "/resume-tries").c_str());
}


static std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
	for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
	return s;
}

// ---- the Source with a few host settings on top -------------------------------------------------------------------
struct Host : Library
{
	using Library::Library;
	bool motion = true;
	bool topMotion() override { return motion; }
	std::string batteryShown = "icon";		// "icon", or "text": the percentage too
	std::string batteryMode() override { return batteryShown; }
};


struct Options
{
	std::string assets = "/usr/share/banana/rnds", splash = "/usr/share/banana/splash", banana = "/usr/share/banana/banana";
	std::string systems = "/etc/banana/systems.conf", roms = "/data/roms", data = "/data/banana";
	std::string top = "DSI-1";
	std::string headless, script;
	std::string theme;		// --theme FILE: that theme's .json instead of the settings' (a theme being made)
	int w = 640, h = 480;
};

static std::string envOr(const char* name, const std::string& def)
{
	const char* v = getenv(name);
	return v && *v ? v : def;
}

// ---- held buttons repeat, like a list in any frontend -----------------------------------------------------------
struct Repeat
{
	Key key = Key::NONE;
	double since = 0, last = 0;
	bool due(double now)
	{
		if (key == Key::NONE) return false;
		double held = now - since, gap = held > 1500 ? 60 : 120;
		if (held < 400 || now - last < gap) return false;
		last = now;
		return true;
	}
	int wait(double now) const
	{
		if (key == Key::NONE) return -1;
		double held = now - since;
		if (held < 400) return (int)std::ceil(400 - held);
		double gap = held > 1500 ? 60 : 120;
		return (int)std::max(0.0, std::ceil(last + gap - now));
	}
};

static bool uiButton(Key k, Menu::Button& b)
{
	switch (k)
	{
	case Key::LEFT: b = Menu::LEFT; return true;
	case Key::RIGHT: b = Menu::RIGHT; return true;
	case Key::UP: b = Menu::UP; return true;
	case Key::DOWN: b = Menu::DOWN; return true;
	case Key::A: b = Menu::A; return true;
	case Key::B: b = Menu::B; return true;
	case Key::X: b = Menu::X; return true;
	case Key::Y: b = Menu::Y; return true;
	case Key::L: b = Menu::L; return true;
	case Key::R: b = Menu::R; return true;
	default: return false;
	}
}

static const char* soundOf(Key k, Menu::View before, Menu::View after)
{
	switch (k)
	{
	case Key::LEFT: case Key::RIGHT: case Key::UP: case Key::DOWN: case Key::L: case Key::R: return "scroll";
	case Key::A: return after == Menu::LAUNCH && before != Menu::LAUNCH ? "ready" : "select";
	case Key::B: return "back";
	case Key::Y: return "fav";
	case Key::X: return "select";
	default: return nullptr;
	}
}

// The shoulder buttons jump by letter in a game list; the D-pad still steps one game at a time. R: the next title
// letter's first game; L: this letter's first game, or the previous letter's. The list is sorted by title, so a
// letter's games are one run (Library::letterStep). In a library only -- on the home shelf L/R still walk the
// systems -- and true only when it consumed the key (so the caller does not then nudge by one).
static bool letterJump(Menu& ui, Library& lib, Menu::Button b)
{
	if ((b != Menu::L && b != Menu::R) || ui.view() != Menu::LIBRARY || lib.gameCount() <= 1) return false;
	int at = ui.gameIndex();
	int to = lib.letterStep(at, b == Menu::R ? 1 : -1);
	if (to >= 0 && to != at) ui.jumpTo(to);
	return true;
}

// a favourite starred or unstarred: once the shelf shows, the Favorites collection and the favourite systems are put
// right, and the cursor stays on the system it was on
template <class U> static bool rearrange(Host& lib, U& ui)
{
	if (!lib.favouritesChanged() || ui.view() != Menu::HOME) return false;
	int at = ui.systemIndex();
	std::string id = at >= 0 && at < lib.systemCount() ? lib.shelf(at).def.id : "";
	lib.arrange();
	int to = 0;
	for (int i = 0; i < lib.systemCount(); i++)
		if (lib.shelf(i).def.id == id) to = i;
	ui.dataChanged();
	ui.setHome(to);
	return true;
}

// ---- games copied onto the card while the menu runs (file sharing) ------------------------------------------------
// inotify on the ROM folders; the library is read again once the copying has been quiet for 2 s, and only while the
// menu shows its home screen (a game list under the cursor isn't reshuffled)
struct RomWatch
{
	int fd = -1;
	double due = 0;
	std::map<int, std::string> dirOf;	// each watch's folder
	static constexpr uint32_t EVENTS = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
	void add(const std::string& d)
	{
		int wd = inotify_add_watch(fd, d.c_str(), EVENTS);
		if (wd >= 0) dirOf[wd] = d;
	}
	void watch(const std::vector<std::string>& dirs)
	{
		if (fd < 0) fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (fd < 0) return;
		for (auto& d : dirs) add(d);	// a folder that isn't there yet is caught by its parent's watch, then added here
	}
	void read(double now)
	{
		alignas(inotify_event) char buf[4096];
		bool any = false;
		for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;)
		{
			any = true;
			// a folder made or moved in (a game folder copied over file sharing): watched at once, so the games
			// copied into it afterwards are seen too (the rescan 2 s after the folder came may find it still empty)
			for (char* p = buf; p < buf + n;)
			{
				auto* e = (inotify_event*)p;
				auto it = dirOf.find(e->wd);
				if ((e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO)) && e->len && it != dirOf.end() && e->name[0] != '.')
					add(it->second + "/" + e->name);
				p += sizeof(inotify_event) + e->len;
			}
		}
		if (any) due = now + 2000;
	}
};

// ---- boot timing: seconds since the kernel started, for logs/boot.txt (rcS gathers /run/banana/boot-times) ------
static void bootMark(const char* what)
{
	timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	double s = t.tv_sec + t.tv_nsec / 1e9;
	logf("%s at %.2f s after the kernel started", what, s);
	if (FILE* f = fopen("/run/banana/boot-times", "a")) { fprintf(f, "%-28s %6.2f s\n", what, s); fclose(f); }
}

// ---- the low-battery notice ---------------------------------------------------------------------------------------
// At 20%, 10% and 5% on the battery: a notice over the top panel for 8 s, which any
// button hides. Each step once, until the charger has been on; a step passed during a game shows when the menu is
// back. bananad does the shutting down (battery_shutdown_percent); this only says it is coming.
struct LowBattery
{
	int shownStep = 101, percent = -1;
	double until = 0;
	std::string title, body;

	bool showing(double now) const { return until > now; }
	// true when the notice came or went
	bool check(const Library::Power& p, int shutdownAt, double now)
	{
		if (p.percent < 0) return false;
		if (p.charging)
		{
			shownStep = 101;
			return hide(now);
		}
		int step = p.percent <= 5 ? 5 : p.percent <= 10 ? 10 : p.percent <= 20 ? 20 : 0;
		if (!step || step >= shownStep) return false;
		shownStep = step;
		percent = p.percent;
		until = now + 8000;
		std::string off = std::to_string(shutdownAt) + "%";
		if (step == 5)
		{
			title = "Battery nearly empty: " + std::to_string(percent) + "%";
			body = "Save your game now. At " + off + " Banana closes it and shuts down.";
		}
		else
		{
			title = "Battery low: " + std::to_string(percent) + "%";
			body = step == 10 ? "Plug in the charger soon. At " + off + " Banana closes the game and shuts down."
				: "Time to find the charger.";
		}
		return true;
	}
	bool hide(double now)
	{
		bool was = showing(now);
		until = 0;
		return was;
	}
};

// A charger plugged in or out: a short notice ("Charging" / "On battery") on the same overlay as the low-battery one.
// The status bar's battery also grows a bolt while charging (rnds::Source::charging, from the same power_supply);
// bananad plays the vibration.
struct PlugNotice
{
	int seen = -1;	// -1: not known yet (no notice at boot), 0: on battery, 1: charging
	double until = 0;
	std::string title, body;
	int percent = -1;
	bool showing(double now) const { return until > now; }
	bool check(const Library::Power& p, double now)
	{
		int s = p.charging ? 1 : 0;
		if (seen < 0 || s == seen) { seen = s; return false; }
		seen = s;
		until = now + 2500;
		percent = p.percent;
		title = s ? "Charging" : "On battery";
		body = p.percent >= 0 ? std::to_string(p.percent) + (s ? "% and charging" : "% left")
			: (s ? "The charger is in" : "The charger is out");
		return true;
	}
};

static int shutdownPercent(const std::string& data)
{
	return atoi(getConfValue(data + "/bananad.conf", "battery_shutdown_percent",
		getConfValue("/etc/banana/bananad.conf", "battery_shutdown_percent", "3")).c_str());
}

// ---- the charger's uevents -----------------------------------------------------------------------------------------
// The menu asks /sys for the battery once a minute (and after a press), so a cable going in used to sit unnoticed
// until something else redrew the top bar. Watch the kernel's uevent group instead: a power_supply event is the
// charger going in or out, which repaints the battery (and its bolt) and brings the plug notice up at once.
static int powerUeventOpen()
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) return -1;
	struct sockaddr_nl a = {};
	a.nl_family = AF_NETLINK;
	a.nl_groups = 1;	// the kernel's uevents
	if (bind(fd, (struct sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
	return fd;
}

// true when what was queued includes a power_supply event (the rest of the group is dropped: input, mmc...)
static bool powerUeventDrain(int fd)
{
	char buf[8192];
	bool hit = false;
	for (;;)
	{
		ssize_t n = read(fd, buf, sizeof buf - 1);
		if (n <= 0) break;
		buf[n] = 0;
		for (ssize_t i = 0; i < n; )
		{
			const char* s = buf + i;
			size_t l = strnlen(s, (size_t)(n - i));
			if (l && !strncmp(s, "SUBSYSTEM=power_supply", 22)) hit = true;
			i += (ssize_t)l + 1;
		}
	}
	return hit;
}


// headless.cpp: the test harness; boot_anim.cpp: the splash and the iris into the menu
int runHeadless(const Options& o, Host& lib, const std::string& variant, const std::string& theme, double scanMs);
void playSplash(Display& disp, SoftBackend& be, const Options& o, const std::string& variant, bool on,
	std::atomic<bool>& ready, std::mutex& readyM, std::condition_variable& readyCv, Sound& sound, int sig, bool waitAudio);
void forgetSplash();	// free its layers once it is over
void irisToMenu(Display& disp, SoftBackend& be, Menu& ui);
