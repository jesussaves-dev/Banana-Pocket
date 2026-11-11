// banana-ui: Banana's frontend. A JSON layout engine drawn in software straight
// onto the panel: no EmulationStation, no compositor, no GPU. It sleeps in poll() until a button, a
// finished picture or an animation step needs a frame, and gives the panel, the input devices and the sound device
// away completely while a game runs.
//
//   banana-ui [--assets DIR] [--systems FILE] [--roms DIR] [--data DIR]
//   banana-ui --headless OUTPREFIX --script "home 0; wait 2400; shot home" [--size 640x480] ...   (renders PNGs)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

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

#include "app.h"
#include "pause_menu.h"

using namespace banana;

static rnds::Bitmap stbLoad(const std::string& path)
{
	rnds::Bitmap b;
	int w, h, n;
	unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!p) return b;
	b.w = w; b.h = h;
	b.px.assign(p, p + (size_t)w * h * 4);
	stbi_image_free(p);
	return b;
}

// The last pictures decoded, kept a little: one box art makes several pictures, each its own job (Pixel's cartridge
// and cover, the cartridge in the row, lifted and on screen), and decoding the PNG was most of each job's
// time. A file is known by its path, size and time (art scraped again is read again); a picture another worker is
// decoding is waited for, not decoded twice
typedef std::shared_ptr<const rnds::Bitmap> DecodedPic;
struct Decoded { std::string key; std::shared_future<DecodedPic> pic; size_t bytes; };
static std::mutex decodedLock;
static std::list<Decoded> decoded;		// the newest first
static size_t decodedBytes = 0;
static const size_t DECODED_BUDGET = 12u << 20;	// a dozen covers or so (a 500x450 one is 0.9 MB)

// (a game is starting: the memory is the game's)
static void forgetDecoded()
{
	std::lock_guard<std::mutex> l(decodedLock);
	decoded.clear();
	decodedBytes = 0;
}

static rnds::Bitmap stbDecode(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0) return rnds::Bitmap();
	std::string key = path + '\0' + std::to_string((long long)st.st_size) + ':' + std::to_string((long long)st.st_mtime) + '.' +
		std::to_string((long)st.st_mtim.tv_nsec);
	auto same = [&](const Decoded& e) { return e.key == key; };
	std::shared_future<DecodedPic> pic;
	std::promise<DecodedPic> mine;
	bool decode = false;
	{
		std::lock_guard<std::mutex> l(decodedLock);
		auto it = std::find_if(decoded.begin(), decoded.end(), same);
		if (it != decoded.end())
		{
			decoded.splice(decoded.begin(), decoded, it);
			pic = it->pic;
		}
		else
		{
			pic = mine.get_future().share();
			decoded.push_front(Decoded{ key, pic, 0 });
			decode = true;
		}
	}
	if (decode)
	{
		auto b = std::make_shared<const rnds::Bitmap>(stbLoad(path));
		mine.set_value(b);
		std::lock_guard<std::mutex> l(decodedLock);
		auto it = std::find_if(decoded.begin(), decoded.end(), same);
		if (it != decoded.end()) { it->bytes = b->px.size(); decodedBytes += it->bytes; }
		while (decodedBytes > DECODED_BUDGET && decoded.size() > 1) { decodedBytes -= decoded.back().bytes; decoded.pop_back(); }
	}
	return *pic.get();
}

// settings.conf: key = value lines, the image's defaults under the card's own (settings_defaults.cpp)

// ---- the workers that decode and fit pictures off the main thread -----------------------------------------------
// A few threads (one fewer than the cores, at most three), so scrolling through a big library keeps up with its
// pictures; niced, so the frames' thread is never the one kept waiting for a core
struct Worker
{
	std::shared_ptr<Menu> ui;	// (shared: a menu doing its own locking is kept alive by the job running without uiLock)
	std::mutex uiLock, wlock;
	std::condition_variable cv;
	bool stop = false;
	uint64_t kicks = 0;		// jobs queued: each thread works until the queue is empty once it sees a new one
	int wakeFd = -1;		// eventfd: a job finished, the main loop has something to upload
	std::vector<std::thread> th;

	void start(std::shared_ptr<Menu> u)
	{
		ui = u;
		wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		ui->setNotifiers([this] { kick(); }, [this] { uint64_t one = 1; if (write(wakeFd, &one, 8) < 0) {} });
		int n = std::max(1, std::min(3, (int)std::thread::hardware_concurrency() - 1));
		for (int i = 0; i < n; i++) th.emplace_back([this] { run(); });
	}
	// a new UI (the theme changed): called with uiLock held
	void attach(std::shared_ptr<Menu> u)
	{
		ui = u;
		ui->setNotifiers([this] { kick(); }, [this] { uint64_t one = 1; if (write(wakeFd, &one, 8) < 0) {} });
	}
	void kick()
	{
		{ std::lock_guard<std::mutex> l(wlock); kicks++; }
		cv.notify_all();
	}
	void run()
	{
		setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
		uint64_t seen = 0;
		for (;;)
		{
			{
				std::unique_lock<std::mutex> l(wlock);
				cv.wait(l, [&] { return stop || kicks != seen; });
				if (stop) return;
				seen = kicks;
			}
			for (;;)
			{
				{ std::lock_guard<std::mutex> l(wlock); if (stop) return; }
				std::shared_ptr<Menu> u;
				bool more;
				{
					std::unique_lock<std::mutex> l(uiLock);
					u = ui;
					// a menu that guards its own state (both do: their jobs only share the queue) works without the
					// lock the frames need; one that didn't would work with it
					if (u->ownLocking()) l.unlock();
					more = u->workOnce();
				}
				if (!more) break;
			}
		}
	}
	void finish()
	{
		{ std::lock_guard<std::mutex> l(wlock); stop = true; }
		cv.notify_all();
		for (auto& t : th) if (t.joinable()) t.join();
		if (wakeFd >= 0) close(wakeFd);
	}
};

// ---- signals: a self-pipe ---------------------------------------------------------------------------------------
static int sigPipe[2] = { -1, -1 };
static void onSignal(int sig)
{
	char c = (char)sig;
	int e = errno;
	if (write(sigPipe[1], &c, 1) < 0) {}
	errno = e;
}

// The CPU's lowest clock raised to the menu's highest (banana-perf ui: schedutil up to 1416 MHz) from a press and while
// frames change the panel, and back 300 ms after the last one. schedutil only ramps up after an animation has started, so its first
// frames were drawn at the lowest clock, slower than the refresh allows (a
// scroll showed frames late). Starting a game puts it back as well (banana-perf sets the lowest clock).
struct ClockBoost
{
	// one entry per cluster the SoC has: policy0 (the six Cortex-A55) and policy6 (the two Cortex-A78). Boosting
	// only policy0 leaves the cores that actually draw the frames on the ramping cluster idle, so a scroll was still late.
	struct Cluster { std::string dir, low; };
	std::vector<Cluster> clusters;
	bool on = false;
	double lastFrame = 0;
	void write(const std::string& dir, const char* file, const std::string& v) { std::ofstream(dir + "/" + file) << v << "\n"; }
	std::string read(const std::string& dir, const char* file) { std::ifstream f(dir + "/" + file); std::string v; f >> v; return v; }
	void list()
	{
		if (!clusters.empty()) return;
		DIR* d = opendir("/sys/devices/system/cpu/cpufreq");
		if (!d) return;
		struct dirent* e;
		while ((e = readdir(d)))
			if (!strncmp(e->d_name, "policy", 6))
				clusters.push_back({ std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name, std::string() });
		closedir(d);
	}
	void frame(double now)
	{
		lastFrame = now;
		if (on) return;
		list();
		std::vector<Cluster> raised;
		for (auto& c : clusters)
		{
			if (read(c.dir, "scaling_governor") != "schedutil") continue;	// a game's profile, or no cpufreq: leave it
			c.low = read(c.dir, "scaling_min_freq");
			std::string high = read(c.dir, "scaling_max_freq");
			if (c.low.empty() || high.empty()) continue;
			write(c.dir, "scaling_min_freq", high);
			raised.push_back(c);
		}
		if (raised.empty()) return;
		clusters = raised;	// only what was raised is the thing to put back
		on = true;
	}
	void idle(double now) { if (on && now - lastFrame > 300) release(); }
	void release() { if (!on) return; for (auto& c : clusters) write(c.dir, "scaling_min_freq", c.low); on = false; }
	int wake(double now) const { return on ? std::max(1, (int)(lastFrame + 301 - now)) : -1; }	// ms until idle() has work
};

// BANANA_FRAME_STATS=1: a log line for each second the menu presented frames: how many, the gaps between presents in
// refreshes, and the time to draw a frame and to copy it to the panels (for judging the menu's smoothness on a device)
struct FrameStats
{
	bool on = getenv("BANANA_FRAME_STATS") != nullptr;
	double start = 0, last = 0, drawSum = 0, drawMax = 0, copySum = 0, copyMax = 0;
	int frames = 0, draws = 0, gap[4] = {};
	void draw(double ms) { if (!on) return; draws++; drawSum += ms; drawMax = std::max(drawMax, ms); }
	void present(double t, double copyMs)
	{
		if (!on) return;
		if (last > 0 && t - last < 500) { int g = (int)((t - last) / 16.667 + 0.5); gap[std::min(3, std::max(0, g - 1))]++; }
		if (start == 0) start = t;
		last = t;
		frames++; copySum += copyMs; copyMax = std::max(copyMax, copyMs);
		if (t - start >= 1000)
		{
			logf("frames: %d in %.0f ms, gaps 1:%d 2:%d 3:%d 4+:%d refreshes, draw %.1f/%.1f ms, copy %.1f/%.1f ms (avg/max)",
				frames, t - start, gap[0], gap[1], gap[2], gap[3], draws ? drawSum / draws : 0, drawMax, copySum / frames, copyMax);
			*this = FrameStats();
		}
	}
};

// One tab-separated line, split into exactly n fields (empty ones when the line is short). What RetroArch's patch
// writes for an achievement (buildroot/patches/retroarch/0013) is one such line, and a title can hold anything but a
// tab, so splitting is all the parsing there is.
static std::vector<std::string> tsv(const std::string& line, size_t n)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (out.size() + 1 < n)
	{
		size_t tab = line.find('\t', start);
		if (tab == std::string::npos) break;
		out.push_back(line.substr(start, tab - start));
		start = tab + 1;
	}
	out.push_back(line.substr(start));
	while (out.size() < n) out.push_back(std::string());
	return out;
}

// the environment, then the command line; false for one that makes no sense
static bool parseOptions(int argc, char** argv, Options& o)
{
	o.assets = envOr("BANANA_ASSETS", o.assets);
	o.systems = envOr("BANANA_SYSTEMS", o.systems);
	o.roms = envOr("BANANA_ROMS", o.roms);
	o.data = envOr("BANANA_DATA", o.data);
	o.top = envOr("BANANA_TOP", o.top);
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next = [&]() -> std::string { if (i + 1 >= argc) { logf("%s needs a value", a.c_str()); exit(2); } return argv[++i]; };
		if (a == "--assets") o.assets = next();
		else if (a == "--splash") o.splash = next();
		else if (a == "--banana") o.banana = next();
		else if (a == "--systems") o.systems = next();
		else if (a == "--roms") o.roms = next();
		else if (a == "--data") o.data = next();
		else if (a == "--headless") o.headless = next();
		else if (a == "--script") o.script = next();
		else if (a == "--theme") o.theme = next();
		else if (a == "--size") { std::string s = next(); if (sscanf(s.c_str(), "%dx%d", &o.w, &o.h) != 2) { logf("bad size"); return false; } }
		else { logf("unknown option %s", a.c_str()); return false; }
	}
	return true;
}

int main(int argc, char** argv)
{
	Options o;
	if (!parseOptions(argc, argv, o)) return 2;

	rnds::setDecoder(stbDecode);
	// the card's settings.conf made whole (missing keys added, invalid values put back): first boot and after an update
	normalizeSettings(o.data);
	auto settings = readSettings(o.data);
	i18nInit(o.data);	// the Settings screen's language, before it is built
	if (!o.theme.empty()) settings["theme"] = "file:" + o.theme;	// --theme: the theme being made (headless render)
	// banana/themes on the card, for themes of one's own: the image's README and examples to start from (once)
	if (access((o.data + "/themes").c_str(), F_OK) != 0 && access("/usr/share/banana/themes", F_OK) == 0)
		if (system(("mkdir -p '" + o.data + "/themes' && cp -r /usr/share/banana/themes/. '" + o.data + "/themes/' 2>/dev/null").c_str()) != 0) {}	// (-r: a theme's folder of pictures too)
	// the time zone, for the clock and every game started from here (a POSIX rule, see settings.cpp)
	if (!settings["timezone"].empty()) { setenv("TZ", settings["timezone"].c_str(), 1); tzset(); }
	// the palette (a user theme's colours and accent set on the way) and the accent, before anything is drawn: the
	// splash and the menu in it
	std::string themeWhy, variant = applyTheme(settings["theme"], settings["accent"], &themeWhy);
	if (!themeWhy.empty()) logf("theme: %s (dark instead)", themeWhy.c_str());
	// beside the logo: the release stage and the version ("0.3.0-dev" -> "Alpha 0.3.0"; /etc/banana/stage names
	// another stage)
	{
		std::string v, stage = "Alpha";
		std::ifstream("/etc/banana/version") >> v;
		std::ifstream sf("/etc/banana/stage"); std::string st; if (std::getline(sf, st) && !st.empty()) stage = st;
		if (!v.empty()) rnds::brandNote = stage + " " + v.substr(0, v.find('-'));
	}
	Host lib(o.systems, o.roms, o.data, o.assets);
	lib.motion = settings["motion"] != "off";
	lib.twelveHour = settings["clock"] == "12h";
	lib.batteryShown = settings["battery"] == "percent" ? "text" : "icon";

	if (!o.headless.empty())
	{
		double t0 = nowMs();
		lib.scan();
		return runHeadless(o, lib, variant, settings["theme"], nowMs() - t0);
	}

	if (pipe2(sigPipe, O_CLOEXEC | O_NONBLOCK)) return 1;
	struct sigaction sa = {};
	sa.sa_handler = onSignal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGUSR1, &sa, nullptr);	// bananad after a resume: draw everything again
	sigaction(SIGUSR2, &sa, nullptr);	// bananad after a volume or brightness key: the level card (/run/banana/osd)
	sigaction(SIGHUP, &sa, nullptr);	// bananad before a sleep: hand the sound device over, stop the ambient loop
	signal(SIGPIPE, SIG_IGN);

	// the sound device: opened as early as possible, before the panel, so the boot splash's first cue (the bunch's
	// insert) finds the AudioReach card already open and lands on the animation's frame instead of late
	Sound sound(o.assets + "/sounds");
	sound.enabled = settings["sounds"] != "off";
	sound.setBgm(settings["bgm"] != "off");		// the ambient loop, if the card or the image has one
	if (settings["splash"] != "off" && settings["sounds"] != "off") sound.warm();

	// Phase 3A: the compositor owns the display (atomic KMS when available); the main loop stays here and is
	// the persistent loop the Wayland dispatch joins in 3B.
	Compositor comp;
	Display& disp = comp.display();
	std::string err;
	// the panel's driver may still be probing when init starts us
	for (int tries = 0; !disp.openSingle(o.top, err); tries++)
	{
		if (tries >= 100) { logf("%s", err.c_str()); return 1; }
		sleepUs(50000);
	}
	SoftBackend be(disp.width(), disp.height());
	logf("panel %dx%d", disp.width(), disp.height());
	bootMark("banana-ui: panel open");

	FrameStats fstat;
	ClockBoost boost;
	lib.onCue = [&sound](const std::string& n) { sound.play(n); };

	// the library and the menu are made on another thread while the boot animation plays
	std::shared_ptr<Menu> uiPtr;
	// a 640-wide layout, and the panel's extra height below it
	const float sc = disp.width() / 640.0f;
	const float canvasW = disp.width() / sc, canvasH = disp.height() / sc;
	// the theme's menu
	auto makeMenu = [&](const std::string& themeIn, float scv) -> Menu*
	{
		std::string theme = themeIn, layout = themePath(themeIn);
		if (!isLayoutTheme(layout))
		{
			theme = pocketTheme();	// a colour theme on its own: Pocket's layout, in its palette
			layout = themePath(theme);
		}
		applyTheme(theme, settings["accent"]);
		return new LayoutMenu(&lib, &be, layout, scv, o.assets + "/fonts", o.banana + "/fonts", canvasW, canvasH);
	};
	// The theme the menu actually draws with. On the one panel a colour theme (not a layout) is forced to
	// the Pocket layout, so Settings has to follow *that* value, not the raw setting: otherwise the menu shows Pocket
	// and Settings shows the colour theme's palette, and the two disagree.
	auto menuTheme = [&]() -> std::string
	{
		std::string t = settings["theme"], l = themePath(t);
		if (!isLayoutTheme(l)) return pocketTheme();
		return t;
	};
	std::atomic<bool> ready{ false };
	std::mutex readyM;
	std::condition_variable readyCv;
	std::thread loader([&]
	{
		double t0 = nowMs();
		lib.scan();
		logf("library: %d shelves in %.0f ms", lib.systemCount(), nowMs() - t0);
		int startSys = 0;
		{
			std::ifstream f(o.data + "/last-system");
			f >> startSys;
			if (startSys < 0 || startSys >= lib.systemCount()) startSys = 0;
		}
		uiPtr.reset(makeMenu(settings["theme"], sc));
		uiPtr->setHome(startSys);
		// the menu's pictures decoded here too, while the animation plays (on the main thread at the hand-over they
		// stopped the screens for ~200 ms)
		uiPtr->update(nowMs());
		uiPtr->finishWork();
		ready = true;
		readyCv.notify_all();
	});
	// A resume skips the boot animation: the game is the point, and the splash is a second or two of transition.
	std::string resumeRom;
	Resume resume = resumePlan(o.data, resumeRom);
	if (resume == Resume::Ready) logf("resume: ready %s (any key cancels)", resumeRom.c_str());
	else if (resume != Resume::None) resumeCancel(o.data);
	playSplash(disp, be, o, variant, settings["splash"] != "off" && resume != Resume::Ready, ready, readyM, readyCv, sound, sigPipe[0],
	           settings["sounds"] != "off");
	// the splash is done: free its layers (bgTop is a full screen) before the menu's idle loop, so they are not held
	// for the whole session (the idle RSS budget)
	forgetSplash();
	loader.join();
	Menu* ui = uiPtr.get();
	ui->setLog([](const std::string& m) { logf("%s", m.c_str()); });
	Worker worker;
	worker.start(uiPtr);
	irisToMenu(disp, be, *ui);
	bootMark("banana-ui: the menu");
	// the first boot after the installation: the setup (Wi-Fi, the time zone, the accounts) before anything else
	bool firstSetup = access((o.data + "/setup-done").c_str(), F_OK) != 0;

	Input input;
	input.open(disp.width(), disp.height(), disp.rotation(), disp.panelWidth(), disp.panelHeight());

	// Phase 3B: the in-process Wayland server (one kiosk client: the game). It needs the atomic
	// display, so a legacy panel logs why it is off and the menu runs exactly as before.
	if (!comp.start(err)) logf("compositor: off: %s", err.c_str());
	// 3E.2: keyboard-class devices reach a client on the primary; the pad and the power/volume keys stay direct.
	bool forwardOn = false;
	input.raw = [&](int type, int code, int value, InputClass cls)
	{
		if (cls == InputClass::KEYBOARD) comp.forwardInput(type, code, value);
	};

	Repeat rep;
	bool dirty = true, quit = false, drawn = false;	// drawn: a frame on the canvas waiting for the flips
	bool gameWas = false;	// a Wayland client owned the primary last pass: the menu is restored when it goes
	double quietUntil = 0;	// no frame before this: the last one flipped nothing (see present below)
	// the AMOLED burn-in guard: the panel's backlight, owned here while the menu runs (bananad's own idle dim leaves
	// it to us while /run/banana/panel-ui is there). IdleGuard does the dim/blank/wake; attached just below.
	IdleGuard guard;
	std::string panelBl, panelBlMax;
	bool swallowTouch = false;	// the press that woke the panel: its touch-up must not land either
	int lastMinute = -1;
	float scale = sc;
	Settings settingsView(&be, o.assets, o.data, scale, variant, &lib);
	// the night light is the panel's own gamma, set before any game is handed the panel
	std::function<void()> nightLight = [&]() {
		std::string v = getConfValue(o.data + "/settings.conf", "night_light",
		                             getConfValue("/etc/banana/settings.conf", "night_light", "off"));
		disp.setWarmth(v == "warmer" ? 2 : v == "warm" ? 1 : 0);
	};
	nightLight();
	// the system shader's saved mask and colour, set before the first flip like the night light
	std::string shaderMaskSet, shaderColorSet;	// what the panel has, so a slider move stays one integer
	MaskGeom shaderGeom;		// the mask's cell for what is on screen: a game's, or the default in the menu
	bool shaderChordOff = false;	// MENU + Select took the mask away for this game, unsaved
	FrameHole shaderHole;		// the frame on screen's hole, to hand back to the compositor after a re-bake
	auto shaderMaskOf = [](const std::string& m) {
		return m == "crt240" ? SHADER_MASK_CRT_240P : m == "aperture" ? SHADER_MASK_APERTURE_GRILLE :
		       m == "lcd" ? SHADER_MASK_LCD_GRID : SHADER_MASK_OFF;
	};
	std::function<void(bool)> shaderApply = [&](bool force) {
		auto conf = [&](const char* k, const char* d) {
			return getConfValue(o.data + "/settings.conf", k, getConfValue("/etc/banana/settings.conf", k, d));
		};
		std::string m = conf("shader_mask", "off"), c = conf("shader_color", "bypass");
		disp.shaderSetIntensity(atoi(conf("shader_mask_intensity", "50").c_str()));
		if (force || m != shaderMaskSet)
			disp.shaderSetMask(shaderMaskOf(m), shaderGeom);
		if (force || c != shaderColorSet)
			disp.shaderSetColor(c == "trinitron" ? COLOR_PROFILE_TRINITRON_9300K : c == "pvm" ? COLOR_PROFILE_PVM_6500K :
			                    c == "handheld" ? COLOR_PROFILE_HANDHELD_LCD : COLOR_PROFILE_BYPASS);
		shaderMaskSet = m;
		shaderColorSet = c;
	};
	shaderApply(true);
	settingsView.setPanelHeight(canvasH);
	settingsView.searchCount = [&](const std::string& q) { return lib.countMatches(q); };
	settingsView.setVariant(menuTheme());
	settingsView.setOverride = [&](const std::string& s, const std::string& r, const std::string& c, const std::string& sh, const std::string& f)
		{ lib.setOverride(s, r, c, sh, f); };
	settingsView.setFrameSystems(lib.frameSystems());
	LowBattery low;
	PlugNotice plug;
	bool batteryDue = true;
	if (firstSetup) { settingsView.setup(); logf("first-time setup"); }
	RomWatch roms;
	int powerFd = powerUeventOpen();	// the charger's uevents: repaint at once (see powerUeventOpen)
	roms.watch(lib.watchDirs());
	// the volume and brightness keys' level card, 1.5 s from the last key (bananad changes the level and says so)
	std::string osdLabel;
	int osdPercent = 0;
	double osdUntil = 0;
	// the pause menu (MENU over a game on the compositor) and the game it freezes
	// what the menu knows of RetroArch (it cannot be asked: a query on its stdin crashes it): the save slot RetroArch
	// is on and the one picked here, the disc, fast-forward; all set at launch and changed only through this menu
	int raSlot = 0, discs = 1, discNow = 1;
	bool ffOn = false;
	std::string gameRom;
	// Lines for RetroArch's stdin, sent one at a time ~100 ms apart while it runs: it keeps one flag per command name
	// per poll, so two STATE_SLOT_PLUS (or the eject/next/eject of a disc change) in one frame would count once
	std::deque<std::string> raQueue;
	double raNextAt = 0;
	pid_t gamePid = -1;
	double gameStart = 0;
	int gameIndex = -1;
	bool gameRA = false;
	FrameKind frameKind = FrameKind::None;
	bool gameFramable = false;
	PauseMenu pause(lib, input, comp, gamePid, gameStart, gameIndex, gameRA, gameFramable, gameRom, frameKind, raSlot, discs, discNow, ffOn, raQueue);
	be.overlay = [&]()
	{
		double t = nowMs();
		if (low.showing(t)) settingsView.notice(low.title, low.body, low.percent);
		else if (plug.showing(t)) settingsView.notice(plug.title, plug.body, plug.percent);
		else if (osdUntil > t) settingsView.level(osdLabel, osdPercent);
		else if (pause.open) settingsView.pauseMenu(pause.view);
		else settingsView.drawToast(t);	// the achievement toasts, over whatever page is up
	};

	// 3D cards and the 3E quick menu. showCard draws the menu's own card on a cleared screen and turns only its box
	// into the double-buffered overlay; while a client owns the primary, the overlay carries it.
	bool cardUp = false;
	double cardUntil = 0;
	// draw whatever be.overlay shows now and put its box on the overlay plane for ms
	auto pushCard = [&](const std::string& kind, int ms)
	{
		double t = nowMs();
		if (!comp.clientOnScreen() || !disp.overlayCardBegin()) return;
		be.beginScreen();
		be.endScreen();	// be.overlay draws the level/toast onto the screen
		int cx = 0, cy = 0, cw = 0, ch = 0;
		if (!settingsView.cardRect(cx, cy, cw, ch)) return;
		disp.overlayCardTurn(be.pixels(), cx, cy, cx + cw, cy + ch);
		comp.setOverlayFb(disp.overlayCardFb());
		cardUp = true;
		cardUntil = t + ms;
		logf("overlay card %s for %d ms", kind.empty() ? "?" : kind.c_str(), ms);
	};
	auto showCard = [&](const std::string& kind, const std::string& title, const std::string& body,
	                    const std::string& badge, int ms)
	{
		if (kind == "volume" || kind == "brightness")
		{
			osdLabel = kind == "brightness" ? "Brightness" : "Volume";
			osdPercent = atoi(body.c_str());
			osdUntil = nowMs() + ms;
		}
		else
			settingsView.notify(title, body, 0, badge);
		pushCard(kind, ms);
	};
	auto drawQuick = [&]() { pushCard("pause", 1000000000); };
	// The running game's screen frame, on the overlay plane under any card. Whole-panel work: at a launch or a Frame
	// row change, never per frame. Answers the kind actually shown, None when the art could not be used.
	auto frameArt = [](const std::string& id) { return "/usr/share/banana/overlays/" + id + ".png"; };
	auto frameHasArt = [&](const std::string& id) { return access(frameArt(id).c_str(), R_OK) == 0; };
	auto frameMaster = [&]() { return getConfValue(o.data + "/settings.conf", "bezels", "on") != "off"; };
	auto applyFrame = [&](const std::string& id, FrameKind k) -> FrameKind
	{
		std::vector<uint32_t> px;
		FrameHole hole;
		std::string why;
		const int W = disp.width(), H = disp.height();
		if (k == FrameKind::Art && !frameLoadArt(frameArt(id), W, H, px, hole, why))
		{
			logf("frame: %s", why.c_str());
			k = FrameKind::None;
		}
		else if (k == FrameKind::Banana)
		{
			hole = frameBananaHole(id, W, H);
			frameDrawBanana(px, W, H, hole, settingsView.frameColors());
		}
		// the mask's cell, before the frame is baked over it. An art frame's hole is a whole multiple of the system's
		// picture; a Banana frame turns integer scaling off; with no frame it is RetroArch's own setting.
		{
			int mx = hole.x, my = hole.y, mw = hole.w;
			bool integer = k == FrameKind::Art;
			if (k == FrameKind::None && !id.empty())
			{
				std::string v = getConfValue("/data/retroarch/retroarch.cfg", "video_scale_integer",
				                             getConfValue("/etc/banana/retroarch.cfg", "video_scale_integer", "false"));
				if (v.find("true") != std::string::npos) integer = maskIntegerHole(id.c_str(), W, H, mx, my, mw);
			}
			shaderGeom = maskGeomFor(id.c_str(), mx, my, mw, integer);
			if (shaderMaskSet != "off" && !shaderChordOff)
			{
				disp.shaderSetMask(shaderMaskOf(shaderMaskSet), shaderGeom);
				logf("shader: %s, cell %d at %d,%d", id.c_str(), shaderGeom.rowPeriod, shaderGeom.originX, shaderGeom.originY);
			}
		}
		if (k != FrameKind::None)
		{
			frameDim(px.data(), px.size(), atoi(getConfValue(o.data + "/settings.conf", "frame_brightness", "85").c_str()));
			if (!disp.frameSet(px.data()))
			{
				logf("frame: no buffer for it");
				k = FrameKind::None;
			}
		}
		if (k == FrameKind::None)
		{
			disp.frameSet(nullptr);
			comp.setFrame(0, 0, 0, 0, 0);
			shaderHole = FrameHole();
		}
		else
		{
			comp.setFrame(disp.frameFb(), hole.x, hole.y, hole.w, hole.h);
			shaderHole = hole;
			logf("frame: %s for %s, hole %d,%d %dx%d", frameKindWord(k), id.c_str(), hole.x, hole.y, hole.w, hole.h);
		}
		return k;
	};
	auto hideCard = [&]()
	{
		if (!cardUp) return;
		cardUp = false;
		comp.setOverlayFb(0);
		logf("overlay card hidden");
	};
	// The pause menu: MENU over a game freezes it (SIGSTOP on the launcher's whole tree, so a port or BananaTube stops
	// too) and lists what can be done; RetroArch's own menu stays one row away (and on MENU + X). It replaces the
	// RetroArch menu as the first thing MENU opens: fewer choices, Banana's look, the same for every game.
	// the right panel and the header for the selected row
	pause.drawCard = drawQuick;
	pause.hideCard = hideCard;
	pause.applyFrame = applyFrame;
	pause.frameHasArt = frameHasArt;
	pause.frameMaster = frameMaster;

	// ---- AMOLED burn-in guard: the panel's backlight goes to a night level after 90 s idle and off after 180 s, and
	// the first press or touch restores it and is swallowed (so it can't launch a game or change a setting) ----
	{
		std::string path;
		{ std::ifstream f("/run/banana/backlight-panel"); std::getline(f, path); }
		if (path.empty() || access(path.c_str(), F_OK) != 0)
		{
			if (DIR* d = opendir("/sys/class/backlight"))
			{
				while (dirent* e = readdir(d)) if (e->d_name[0] != '.') { path = std::string("/sys/class/backlight/") + e->d_name; break; }
				closedir(d);
			}
		}
		if (!path.empty()) { panelBl = path + "/brightness"; panelBlMax = path + "/max_brightness"; }
		// tell bananad the menu owns the panel's idle dim; its own dim then leaves this backlight to us
		if (!panelBl.empty()) { std::ofstream mark("/run/banana/panel-ui"); mark << "\n"; }
	}
	guard.attach(panelBl, panelBlMax);
	auto configureGuard = [&]
	{
		// the same file bananad reads; Settings > Sleep & power owns dim_minutes
		guard.configure(atoi(getConfValue(o.data + "/bananad.conf", "dim_minutes", "2").c_str()),
			atoi(getConfValue(o.data + "/bananad.conf", "dim_percent", "30").c_str()));
	};
	configureGuard();
	guard.input(nowMs());

	// what a change on the settings screen means for the menu (called with uiLock held)
	auto applySettings = [&](Settings::Result r)
	{
		switch (r)
		{
		case Settings::MOVED: sound.play("scroll"); break;
		case Settings::CHANGED:
		{
			settings = readSettings(o.data);
			lib.motion = settings["motion"] != "off";
			lib.twelveHour = settings["clock"] == "12h";
			lib.batteryShown = settings["battery"] == "percent" ? "text" : "icon";
			sound.enabled = settings["sounds"] != "off";
			sound.setBgm(settings["bgm"] != "off");
			configureGuard();
			ui->dataChanged();
			sound.play("select");
			break;
		}
		case Settings::THEME:
		{
			// the menu again in the other palette, on the system it was showing
			settings = readSettings(o.data);
			variant = applyTheme(settings["theme"], settings["accent"]);
			int sys = ui->systemIndex();
			uiPtr.reset(makeMenu(settings["theme"], scale));
			ui = uiPtr.get();
			ui->setLog([](const std::string& m) { logf("%s", m.c_str()); });
			worker.attach(uiPtr);
			ui->setHome(sys);
			settingsView.setVariant(menuTheme());	// the menu may have been forced to the Pocket layout above
			sound.play("select");
			break;
		}
		case Settings::CLOSED: logf("settings closed"); sound.play("back"); break;
		case Settings::SEARCH:
		{
			// the Search collection first on the shelf, opened on its first game (an empty text takes it away)
			lib.setSearch(settingsView.searchText);
			ui->dataChanged();
			int s = lib.searchShelf();
			if (s >= 0) { lib.openLibrary(s); ui->setLibrary(s, 0); }
			else ui->setHome(0);
			logf("search \"%s\": %d games", settingsView.searchText.c_str(), s >= 0 ? (int)lib.shelf(s).games.size() : -1);
			sound.play("select");
			break;
		}
		case Settings::REBOOT: logf("restarting"); kill(1, SIGTERM); break;		// busybox init: SIGTERM reboots
		case Settings::POWEROFF: logf("shutting down"); kill(1, SIGUSR2); break;	// and SIGUSR2 powers off
		default: break;
		}
		dirty = true;
	};

	double resumeDue = 0;	// the end of the resume's any-key cancel window (0: none pending)
	auto handle = [&](const InputEvent& ev)
	{
		// a volume key is bananad's (the level, or the backlight with MENU held): here it only cancels the pause menu
		// a held MENU would open on its release, and nothing else in the menu sees it
		if (ev.key == Key::VOLUME) { if (ev.type == InputEvent::PRESS) pause.pending = false; return; }
		// The first press or touch stays in the menu -- except the power key: the press (and release) that turned the
		// console on arrives here too, and cancelling on it made every real boot skip the resume.
		if (resumeDue > 0 && (ev.type == InputEvent::PRESS || ev.type == InputEvent::TOUCH_DOWN) && ev.key != Key::WAKE)
		{
			resumeCancel(o.data);
			logf("resume: cancelled (%s)", ev.type == InputEvent::TOUCH_DOWN ? "touch" : "a key");
			resumeDue = 0;
		}
		// the AMOLED guard: the first press or touch wakes the panel and is swallowed -- and, for a touch, its later
		// up too, so waking the screen can't launch a game or change a setting
		if (swallowTouch && (ev.type == InputEvent::TOUCH_MOVE || ev.type == InputEvent::TOUCH_UP))
		{
			if (ev.type == InputEvent::TOUCH_UP) swallowTouch = false;
			return;
		}
		if (ev.type == InputEvent::PRESS || ev.type == InputEvent::TOUCH_DOWN || ev.type == InputEvent::TOUCH_MOVE)
		{
			double t = nowMs();
			if (ev.type == InputEvent::PRESS || ev.type == InputEvent::TOUCH_DOWN) boost.frame(t);	// what it starts drawn fast
			// the AMOLED guard: the first press or touch while dimmed/blanked wakes the panel and is swallowed
			if (guard.wake(t, &sound)) { swallowTouch = ev.type == InputEvent::TOUCH_DOWN; sound.setBgm(settings["bgm"] != "off"); dirty = true; return; }
			guard.input(t);
		}
		if ((ev.type == InputEvent::PRESS || ev.type == InputEvent::TOUCH_DOWN) && low.hide(nowMs())) dirty = true;
		// 3E: while a client owns the primary the menu does not act on input. MENU opens the quick menu after its
		// release (so the opening press is not grabbed); while it is up only B closes it and the game sees nothing.
		if (comp.clientOnScreen())
		{
			if (pause.open)
			{
				if (ev.type == InputEvent::PRESS) pause.key(ev.key);
				return;
			}
			if (forwardOn && (ev.type == InputEvent::TOUCH_DOWN || ev.type == InputEvent::TOUCH_MOVE || ev.type == InputEvent::TOUCH_UP))
				comp.forwardTouch(ev.type == InputEvent::TOUCH_DOWN ? 0 : ev.type == InputEvent::TOUCH_MOVE ? 1 : 2,
				                  (int)ev.x, (int)ev.y, ev.pointer);
			if (ev.type == InputEvent::PRESS && ev.key == Key::MENU) { pause.pending = true; return; }
			// MENU + Select: the mask off and on for this game, not saved
			if (pause.pending && ev.type == InputEvent::PRESS && ev.key == Key::SELECT)
			{
				pause.pending = false;
				if (shaderMaskSet != "off")
				{
					shaderChordOff = !shaderChordOff;
					disp.shaderSetMask(shaderChordOff ? SHADER_MASK_OFF : shaderMaskOf(shaderMaskSet), shaderGeom);
					comp.setFrame(disp.frameFb(), shaderHole.x, shaderHole.y, shaderHole.w, shaderHole.h);
					logf("shader: mask %s by the chord", shaderChordOff ? "off" : "on");
				}
				return;
			}
			// another button while MENU is held is a RetroArch hotkey (MENU + Start quits, + X its own menu, ...): no
			// pause menu on that release
			if (pause.pending && ev.type == InputEvent::PRESS) { pause.pending = false; return; }
			if (pause.pending && ev.type == InputEvent::RELEASE && ev.key == Key::MENU) { pause.show(); return; }
			return;
		}
		pause.pending = false;
		if (settingsView.isOpen())
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (ev.type == InputEvent::TOUCH_DOWN) settingsView.touch(ev.x, ev.y, true);
			else if (ev.type == InputEvent::TOUCH_UP) applySettings(settingsView.touch(ev.x, ev.y, false));
			else if (ev.type == InputEvent::RELEASE) { if (rep.key == ev.key) rep.key = Key::NONE; }
			else if (ev.type == InputEvent::PRESS)
			{
				applySettings(settingsView.press(ev.key));
				if (ev.key == Key::UP || ev.key == Key::DOWN || ev.key == Key::LEFT || ev.key == Key::RIGHT)
				{
					rep.key = ev.key; rep.since = rep.last = nowMs();
				}
			}
			return;
		}
		if (ev.type == InputEvent::PRESS && ev.key == Key::SELECT)
		{
			// SELECT: the keyboard, to search the games (and SELECT again closes it: see pressKeyboard)
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (ui->view() == Menu::LAUNCH) return;
			ui->touchCancel();
			settingsView.setVariant(menuTheme());
			settingsView.openSearch(lib.searchQuery());
			logf("press select: search open");
			rep.key = Key::NONE;
			sound.play("select");
			dirty = true;
			return;
		}
		if (ev.type == InputEvent::PRESS && ev.key == Key::START)
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (ui->view() == Menu::LAUNCH) return;
			ui->touchCancel();
			settingsView.setVariant(menuTheme());
			settingsView.open();
			rep.key = Key::NONE;
			sound.play("select");
			dirty = true;
			return;
		}
		if (ev.type == InputEvent::PRESS && ev.key == Key::X && ui->view() == Menu::LIBRARY)
		{
			// X on a game: its own options (the core and shader it runs with), over the library
			std::lock_guard<std::mutex> l(worker.uiLock);
			int g = ui->gameIndex();
			const SystemDef& sd = lib.systemOf(g);
			std::string rel = lib.gameAt(g).rel;
			auto ov = lib.overrideOf(sd.id, rel);
			settingsView.setVariant(menuTheme());
			settingsView.openGame(sd.id, rel, lib.gameAt(g).path, lib.gameAt(g).title, ov.core, ov.shader, ov.frame, lib.framable(g));
			rep.key = Key::NONE;
			sound.play("select");
			dirty = true;
			return;
		}
		if (ev.type == InputEvent::TOUCH_DOWN) { std::lock_guard<std::mutex> l(worker.uiLock); dirty |= ui->touch(ev.x, ev.y, true); return; }
		if (ev.type == InputEvent::TOUCH_MOVE) { std::lock_guard<std::mutex> l(worker.uiLock); dirty |= ui->touchMove(ev.x, ev.y); return; }
		if (ev.type == InputEvent::TOUCH_UP) { std::lock_guard<std::mutex> l(worker.uiLock); dirty |= ui->touch(ev.x, ev.y, false); return; }
		if (ev.type == InputEvent::RELEASE) { if (rep.key == ev.key) rep.key = Key::NONE; return; }
		Menu::Button b;
		if (!uiButton(ev.key, b)) return;
		double now = nowMs();
		std::lock_guard<std::mutex> l(worker.uiLock);
		ui->update(now);
		auto before = ui->view();
		if (letterJump(*ui, lib, b) || ui->press(b))
		{
			dirty = true;
			if (const char* s = soundOf(ev.key, before, ui->view())) sound.play(s);
		}
		if (ev.key == Key::LEFT || ev.key == Key::RIGHT || ev.key == Key::UP || ev.key == Key::DOWN || ev.key == Key::L || ev.key == Key::R)
		{
			rep.key = ev.key; rep.since = rep.last = now;
		}
	};

	// a game is a client of this process: the menu loop reaps it and runs this
	auto finishGame = [&](int status)
	{
		pause.close();	// a game that ends (or crashes) under the pause menu must not leave the pad grabbed
		if (shaderChordOff)	// the chord lasts one game: the saved mask is back in the menu
		{
			shaderChordOff = false;
			disp.shaderSetMask(shaderMaskOf(shaderMaskSet), shaderGeom);
		}
		int game = gameIndex;
		long secs = (long)((nowMs() - gameStart) / 1000);
		if (system("banana-perf ui") != 0) {}
		logf("game ended after %ld s (status %d)", secs, status);
		unlink("/run/banana/game.pid");
		unlink("/run/banana/game.path");
		be.invalidate();	// the compositor kept the panel; the menu is drawn again when the client goes
		drawn = false;
		dirty = true;
		std::lock_guard<std::mutex> l(worker.uiLock);
		{
			const char* PROGRESS = "/run/banana/ra-progress";	// "<unlocked> <total>", from the emulator (docs/EMULATORS.md)
			const char* CHEEVOS  = "/run/banana/cheevos";		// one line per unlock, from the same patch
			int achA = -1, achB = 0;
			std::ifstream(PROGRESS) >> achA >> achB;
			lib.recordPlay(game, secs, achA, achB);	// (a start that failed straight away isn't a play)
			// what the game unlocked, one line per achievement: the menu's toast is built from these, and the badge PNG
			// is drawn in the tile. A game that unlocked nothing wrote nothing; a line that is short is a line that was
			// not written at all (the patch always writes all seven fields), so the defaults stand.
			// Bananad showed these as they unlocked (the marker), so the menu only clears it; without the marker
			// (HUD off, or a daemon that predates it) the menu's own toasts are the only place they appear.
			if (access("/run/banana/hud-live", F_OK) != 0)
			{
				std::ifstream ch(CHEEVOS);
				std::string line;
				while (std::getline(ch, line))
				{
					std::vector<std::string> f = tsv(line, 7);
					if (f[0].empty() || f[1].empty()) continue;
					settingsView.notify(f[2], f[3], 0, f[6]);
				}
			}
			unlink("/run/banana/hud-live");
		}
		ui->dataChanged();
		ui->setLibrary(lib.currentShelf(), game);
		sound.setBgm(settings["bgm"] != "off");	// the ambient loop comes back with the menu
		sound.warm();				// the game handed the DSP frontend back: have it open before the next press
		// a game that ended because it was quit goes back to a menu the next boot should show too; one a shutdown
		// stopped leaves the marker bananad wrote, for the next boot to resume
		if (access("/run/banana/shutting-down", F_OK) != 0) resumeCancel(o.data);
		guard.input(nowMs());			// the game's own time doesn't count as menu idle
		dirty = true;
		batteryDue = true;	// a step passed during the game shows now
		gamePid = -1;
		gameIndex = -1;
		gameRA = false;
		gameFramable = false;
		frameKind = applyFrame("", FrameKind::None);
	};

	// The game alone owns the panel while it runs: banana-ui draws nothing, and the game sizes and rotates its own
	// picture. The menu is drawn afresh when it comes back.
	auto runGame = [&](int game)
	{
		const SystemDef& sd = lib.systemOf(game);
		std::string rom = lib.gameAt(game).path;
		std::string cmd = replaceAll(replaceAll(sd.run, "%ROM%", shellQuote(rom)), "%SYSTEM%", shellQuote(sd.id));
		// every launcher runs as a client of this process (the EGL presenter letterboxes and rotates any window size)
		logf("starting %s (resident): %s", sd.id.c_str(), cmd.c_str());
		{
			std::ofstream f(o.data + "/last-system");
			f << ui->systemIndex() << "\n";
		}
		// everything the game wants goes back to the system: the panel, the buttons, the sound device. banana-ui
		// draws nothing while the game runs; the game alone has the panel until it ends.
		sound.release();
		boost.release();
		forgetDecoded();
		drawn = false;	// the menu is drawn afresh when the panel comes back
		rep.key = Key::NONE;
		// before the fork: the game's first configure has to be the hole's size
		gameFramable = lib.framable(game);
		frameKind = gameFramable ? resolveFrame(frameMaster(), lib.overrideOf(sd.id, lib.gameAt(game).rel).frame,
		                                        getConfValue(o.data + "/settings.conf", "frame." + sd.id, ""), frameHasArt(sd.id))
		                         : FrameKind::None;
		frameKind = applyFrame(sd.id, frameKind);
		if (system(("banana-perf " + shellQuote(sd.perf.empty() ? "balanced" : sd.perf)).c_str()) != 0) {}
		const char* PROGRESS = "/run/banana/ra-progress";	// "<unlocked> <total>", from the emulator (docs/EMULATORS.md)
		const char* CHEEVOS  = "/run/banana/cheevos";		// one line per unlock, from the same patch
		unlink(PROGRESS);
		unlink(CHEEVOS);	// last game's: this game's are written as they are won
		double start = nowMs();
		pid_t pid = fork();
		if (pid < 0)
		{
			// the panel, input and sound were handed over above: take them back rather than pretend a game ran
			logf("fork failed: %s", strerror(errno));
			frameKind = applyFrame(sd.id, FrameKind::None);
			be.invalidate();
			sound.setBgm(settings["bgm"] != "off");
			sound.warm();
			guard.input(nowMs());
			dirty = true;
			return;
		}
		if (pid == 0)
		{
			setsid();
			signal(SIGPIPE, SIG_DFL);
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, nullptr);
			setenv("BANANA_ROM", rom.c_str(), 1);
			setenv("BANANA_SYSTEM", sd.id.c_str(), 1);
			setenv("BANANA_RA_PROGRESS", PROGRESS, 1);
			setenv("BANANA_RA_CHEEVOS", CHEEVOS, 1);	// RetroArch's patch writes both (buildroot/patches/retroarch/0013)
			// a game's own Game options (Settings' X): the core and shader this one game runs with, when it has any
			auto ov = lib.overrideOf(sd.id, lib.gameAt(game).rel);
			if (!ov.core.empty()) setenv("BANANA_CORE", ov.core.c_str(), 1);
			if (!ov.shader.empty()) setenv("BANANA_SHADER", ov.shader.c_str(), 1);
			setenv("BANANA_FRAME", frameKindWord(frameKind), 1);	// run-retroarch: the Banana frame's hole wants no integer scale
			execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
			_exit(127);
		}
		if (pid > 0)
		{
			std::ofstream("/run/banana/game.pid") << pid << "\n";	// bananad: the game to pause before a sleep
			std::ofstream("/run/banana/game.path") << rom << "\n";	// ... and to resume if a shutdown interrupts it
			gameIndex = game;
			gameStart = start;
			gameRA = sd.run.find("run-retroarch") != std::string::npos;
			gameRom = rom;
			raQueue.clear();
			// the pause menu's view of RetroArch: slot 0 (run-retroarch starts every launch there; retroarch.cfg's
			// saved state_slot is not what it starts on), disc 1, fast-forward off
			raSlot = 0;
			discs = discCount(rom);
			discNow = 1;
			ffOn = false;
			gamePid = pid;	// the main loop reaps it; the compositor keeps drawing
		}
	};


	if (system("banana-perf ui") != 0) {}
	// the launch hook (below) wakes the menu when it is written: an idle menu sleeps in poll() until a press otherwise,
	// and a launch written then started its game only at the next press, in the middle of whatever came next
	int launchWatch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (launchWatch >= 0 && inotify_add_watch(launchWatch, "/run/banana", IN_CLOSE_WRITE | IN_MOVED_TO) < 0)
	{
		logf("launch file: can't watch /run/banana: %s", strerror(errno));
		close(launchWatch);
		launchWatch = -1;
	}
	// the launch file is looked at once at startup, then only when the watch fires (a pass otherwise ran a wasted
	// access() on every animated frame); without a watch it is looked at every pass as before
	bool launchSeen = true;
	bool cheevosSeen = true;	// /run/banana/cheevos: the achievements a game just unlocked, read on this pass
	bool shotWanted = false;	// /run/banana/screen.req: write the frame to screen.jpg on this pass (below)
	bool overlaySeen = true;	// /run/banana/overlay: a 3D card over the client on the primary
	if (resume == Resume::Ready) resumeDue = nowMs() + 500;	// a short window: the game is the point
	while (!quit)
	{
		double now = nowMs();
		// a resident game is a client of this process: reap it without blocking the compositor
		if (gamePid > 0)
		{
			int st = 0;
			pid_t r = waitpid(gamePid, &st, WNOHANG);
			if (r == gamePid || (r < 0 && errno == ECHILD)) finishGame(st);
		}
		if (resumeDue > 0 && now >= resumeDue)
		{
			// the launch file the menu already watches; the game then owns the panel as usual
			std::ofstream("/run/banana/launch") << resumeRom << "\n";
			resumeTake(o.data);
			logf("resume: launching %s", resumeRom.c_str());
			resumeDue = 0;
		}
		// the burn-in guard is the menu's: dim after bananad.conf's dim_minutes, blank 90 s later -- but never while a
		// game is on screen (Settings > Sleep & power promises that; a client's picture is not menu idle, and a video
		// would go dark) and never blank it while on the charger (a dark panel on the cable read as a sleep)
		// the pause menu's queued RetroArch lines: one per ~100 ms, only while the game runs (not frozen under the menu)
		if (!raQueue.empty() && !pause.open && now >= raNextAt)
		{
			if (gamePid > 0 && gameRA) { logf("retroarch: %s", raQueue.front().c_str()); retroarchCmd(raQueue.front().c_str()); }
			raQueue.pop_front();
			raNextAt = now + 100;
		}
		guard.setCharging(lib.charging());
		if (!comp.clientOnScreen() && guard.tick(now, &sound) && guard.stage() == 2) sound.setBgm(false);	// blanked: let the sound device go too
		// 3E.2: keys reach the client only while it owns the primary, the quick menu is down and the guard is awake
		bool fwd = comp.clientOnScreen() && !pause.open && guard.stage() == 0;
		if (fwd != forwardOn) { forwardOn = fwd; comp.setInputForward(fwd); }
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			ui->update(now);
		}
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (rearrange(lib, *ui)) dirty = true;
		}
		if (input.rescanDue()) { rep.key = Key::NONE; input.open(disp.width(), disp.height(), disp.rotation(), disp.panelWidth(), disp.panelHeight()); }	// a pad came or went: its release will never arrive
		if (settingsView.isOpen())
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (settingsView.update(now)) dirty = true;	// a Wi-Fi scan came back, a network joined
			if (settingsView.takeLibraryChanged()) roms.due = now;	// box art fetched: the library again, at home
			if (settingsView.takeNightLightChanged()) nightLight();	// the panel's gamma follows the row
			if (settingsView.takeShaderChanged()) shaderApply(false);	// its mask and colour too
		}
		// for testing over SSH: a game's path written to /run/banana/launch starts it, as A on it in the library would
		if (launchSeen || launchWatch < 0)
		{
			launchSeen = false;
			if (access("/run/banana/launch", F_OK) == 0)
			{
				std::string want;
				{
					std::ifstream f("/run/banana/launch");
					std::getline(f, want);
				}
				unlink("/run/banana/launch");
				if (gamePid > 0) { logf("launch file ignored: a game is running"); continue; }
				bool found = false;
				for (int s = 0; s < lib.systemCount() && !found; s++)
					for (int i = 0; i < (int)lib.shelf(s).games.size(); i++)
						if (lib.shelf(s).games[i].path == want && !lib.shelf(s).favorites) { lib.openLibrary(s); lib.launch(i); found = true; break; }
				if (!found) logf("launch file: no game at %s", want.c_str());
			}
		}
		// a system-wide hook: anything that wants the menu to show a pop-up writes /run/banana/toast, one line
		// "<glyph>\t<title>\t<body>" (glyph 0 "i", 1 a tick, 2 a cross), and this reads it. Updates, Wi-Fi, the
		// account check -- whatever -- all write the same file; the achievements a game unlocked are read straight from
		// /run/banana/cheevos after the game, with the badge RetroArch downloaded as its picture.
		if (cheevosSeen || launchWatch < 0)
		{
			cheevosSeen = false;
			if (access("/run/banana/toast", F_OK) == 0)
			{
				std::ifstream f("/run/banana/toast");
				std::string line;
				if (std::getline(f, line))
				{
					std::vector<std::string> g = tsv(line, 3);
					settingsView.notify(g[1], g[2], std::max(0, std::min(2, atoi(g[0].c_str()))));
				}
				unlink("/run/banana/toast");
			}
		}
		// 3D: a card over a client on the primary, from /run/banana/overlay (kind|title|body|badge|duration_ms).
		// The card is drawn by the menu's own renderer (level/toast/notice) on a cleared screen, and only its box is
		// turned into the double-buffered overlay buffer, so the pixels are the menu's at 1x and the rest is clear.
		if (overlaySeen || launchWatch < 0)
		{
			overlaySeen = false;
			if (access("/run/banana/overlay", F_OK) == 0)
			{
				std::ifstream f("/run/banana/overlay");
				std::string line;
				if (std::getline(f, line))
				{
					for (char& c : line) if (c == '|') c = '\t';	// the overlay record is pipe-separated
					std::vector<std::string> g = tsv(line, 5);
					std::string kind = g.empty() ? std::string() : g[0];
					std::string title = g.size() > 1 ? g[1] : std::string();
					std::string body = g.size() > 2 ? g[2] : std::string();
					std::string badge = g.size() > 3 ? g[3] : std::string();
					int ms = g.size() > 4 ? atoi(g[4].c_str()) : 2000;
					if (ms <= 0) ms = 2000;
					showCard(kind, title, body, badge, ms);
				}
				unlink("/run/banana/overlay");
			}
		}
		if (cardUp && now >= cardUntil)
		{
			hideCard();
			// a volume/charger/achievement card over the quick menu: put the pause card back when it expires
			if (pause.open) pause.draw();
		}
		int g = lib.pendingLaunch();
		if (g >= 0)
		{
			lib.clearLaunch();
			if (gamePid > 0) { logf("launch ignored: a game is running"); continue; }
			runGame(g);
			continue;
		}
		if (rep.due(now))
		{
			Menu::Button b;
			std::lock_guard<std::mutex> l(worker.uiLock);
			if (settingsView.isOpen()) applySettings(settingsView.press(rep.key));
			else if (uiButton(rep.key, b) && (letterJump(*ui, lib, b) || ui->press(b))) { dirty = true; sound.play("scroll"); }
		}
		// the clock in the status bar
		time_t wall = time(nullptr);
		int minute = (int)(wall / 60);
		if (minute != lastMinute) { lastMinute = minute; dirty = true; batteryDue = true; }
		if (roms.due > 0 && now >= roms.due && !settingsView.isOpen() &&
		    (ui->view() == Menu::HOME || ui->view() == Menu::LIBRARY))
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			// A scan re-arranges the shelves (favourites first), so the system the menu shows is found again by its
			// id, and in the library the game by its path -- not by the index each had. On the library too, not only
			// home: a port (or a game) installed while PortMaster ran is only on the card when the menu comes back,
			// and the Ports shelf it comes back to is exactly where the new port has to appear.
			const bool home = ui->view() == Menu::HOME;
			std::string id = lib.systemCount() ? lib.shelf(home ? ui->systemIndex() : lib.currentShelf()).def.id : "";
			std::string gpath;
			if (!home && lib.gameCount() && ui->gameIndex() >= 0 && ui->gameIndex() < lib.gameCount())
				gpath = lib.gameAt(ui->gameIndex()).path;
			roms.due = 0;
			lib.scan();
			int at = 0;
			for (int i = 0; i < lib.systemCount(); i++) if (lib.shelf(i).def.id == id) at = i;
			ui->dataChanged();
			if (home) ui->setHome(at);
			else
			{
				lib.openLibrary(at);
				int gi = 0;
				for (int i = 0; i < lib.gameCount(); i++) if (lib.shelf(at).games[i].path == gpath) gi = i;
				ui->setLibrary(at, gi);
			}
			roms.watch(lib.watchDirs());
			logf("the games on the card changed: %d systems", lib.systemCount());
			dirty = true;
		}
		if (batteryDue)
		{
			batteryDue = false;
			if (low.check(lib.power(), shutdownPercent(o.data), now)) dirty = true;
			// over a game the notice goes on the overlay plane (bananad no longer sends a second, "Charger" card)
			if (plug.check(lib.power(), now)) { dirty = true; if (comp.clientOnScreen() && !pause.open) pushCard("charger", 2500); }
		}

		bool want;
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			want = dirty || (settingsView.isOpen() ? settingsView.animating() : (ui->animating() || ui->nextWake() == 0));
		}
		// the next frame is drawn while the last one's flips are still pending (present copies it out, so the canvas is
		// free again at once), and handed over when the panel has flipped: drawing and the wait
		// for the panel's vblank overlap instead of adding up to two refreshes a frame
		if (want && disp.isOpen() && !drawn && !comp.gameActive() && !comp.clientOnScreen() && nowMs() >= quietUntil)
		{
			double t0 = nowMs();
			{
				std::lock_guard<std::mutex> l(worker.uiLock);
				if (settingsView.isOpen()) settingsView.render();
				else ui->render();
			}
			fstat.draw(nowMs() - t0);
			drawn = true;
			dirty = false;
		}
		if (drawn && disp.isOpen() && !disp.flipPending() && !comp.gameActive() && !comp.clientOnScreen())
		{
			double t0 = nowMs();
			bool flipped = disp.present(be.pixels(), be.takeDamage());
			fstat.present(t0, nowMs() - t0);
			drawn = false;
			// a frame that changed nothing on the panels flips nothing, so no flip event paces the next one: it waits a
			// refresh instead (an animation that draws the same pixels, a picture still loading, ran this loop at
			// 400-900 frames a second on one core)
			if (!flipped) quietUntil = t0 + 1000.0 / 60;
			else boost.frame(t0);	// moving: the clock stays up (frames that change nothing don't keep it)
			continue;	// see whether the next frame is due before sleeping
		}

		int timeout;
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			timeout = want || drawn ? -1 : (settingsView.isOpen() ? settingsView.nextWake(now) : ui->nextWake());	// a frame is wanted: the flip event wakes us
		}
		if (want && !drawn && !disp.flipPending())	// ...unless no flip is pending: then the end of the quiet refresh
			timeout = std::max(1, (int)(quietUntil - nowMs() + 0.999));
		boost.idle(nowMs());
		if (!want && !drawn)
		{
			int b = boost.wake(nowMs());
			if (b >= 0 && (timeout < 0 || b < timeout)) timeout = b;
		}
		int toMinute = (int)((60 - wall % 60) * 1000);
		if (timeout < 0 || timeout > toMinute) timeout = want || drawn ? timeout : toMinute;
		int r = rep.wait(now);
		if (r >= 0 && (timeout < 0 || r < timeout)) timeout = r;
		bool rescanCan;
		{
			std::lock_guard<std::mutex> l(worker.uiLock);
			// the two views the rescan above covers: in the library the menu would otherwise sit in poll() until an
			// unrelated key, so a ROM copied in while it is open would not appear on its own
			rescanCan = roms.due > 0 && !settingsView.isOpen() &&
				(ui->view() == Menu::HOME || ui->view() == Menu::LIBRARY);
		}
		if (rescanCan)
		{
			int left = std::max(0, (int)(roms.due - now)) + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}
		if (low.showing(now))
		{
			int left = (int)(low.until - now) + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}
		if (plug.showing(now))
		{
			int left = (int)(plug.until - now) + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}
		if (osdUntil > now)
		{
			int left = (int)(osdUntil - now) + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}
		if (settingsView.toasting())
		{
			// a queued achievement: the menu sleeps in poll() until a press otherwise, and the next toast waits on
			// the one showing -- without this it would not come up until the player moved
			int left = (int)settingsView.toastLeftMs() + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}
		{
			// ms until the burn-in guard's next step (dim after dim_minutes, blank 90 s later). Not while a game is on
			// screen: tick() is skipped then, so an overdue deadline would clamp to 1 ms and spin poll() all game long
			int guardMs = comp.clientOnScreen() ? -1 : guard.nextMs(now);
			if (guardMs >= 0 && (timeout < 0 || guardMs < timeout)) timeout = guardMs;
			if (!raQueue.empty() && !pause.open)
			{
				int q = std::max(1, (int)(raNextAt - now) + 1);	// the next queued RetroArch line
				if (timeout < 0 || q < timeout) timeout = q;
			}
		}
		if (resumeDue > 0)
		{
			// the menu sleeps in poll() until a press: the resume deadline has to wake it, or it waits for one
			int left = std::max(0, (int)(resumeDue - now)) + 1;
			if (timeout < 0 || left < timeout) timeout = left;
		}

		// a fixed array, not a vector: this runs every pass (60/s while the menu animates) and the poll set is at
		// most a handful of fds, so an allocation here was pure churn
		struct pollfd fds[40];
		int nf = 0;
		auto addFd = [&](int fd) { if (fd >= 0 && nf < (int)(sizeof fds / sizeof fds[0])) { fds[nf].fd = fd; fds[nf].events = POLLIN; fds[nf].revents = 0; nf++; } };
		addFd(sigPipe[0]); addFd(worker.wakeFd); addFd(roms.fd); addFd(launchWatch); addFd(powerFd);
		if (disp.isOpen()) addFd(disp.pollFd());
		addFd(comp.wlFd());
		nf = input.fillFds(fds, nf, (int)(sizeof fds / sizeof fds[0]));
		if (timeout == 0) timeout = 1;	// a deadline that is due is handled above; never spin on poll(2)
		comp.flush();	// releases/callbacks/input queued since the last dispatch must go out before we block
		int n = poll(fds, nf, timeout);
		if (n == 0 && !want) dirty = true;	// the engine's next step is due
		if (n <= 0) continue;
		for (int fi = 0; fi < nf; fi++)
		{
			const pollfd& p = fds[fi];
			if (!(p.revents & POLLIN)) continue;
			if (p.fd == sigPipe[0])
			{
				char c;
				while (read(sigPipe[0], &c, 1) == 1)
				{
					if (c == SIGTERM || c == SIGINT) quit = true;
					if (c == SIGHUP) { logf("SIGHUP: handing the sound device over"); sound.release(); }	// bananad is about to sleep
					// after a wake: the kernel has the panels showing our buffers again; a redraw brings the clock and
					// the battery up to date (no modeset: that would run the panels' power-on sequence once more)
					if (c == SIGUSR1)
					{
						// bananad after a resume: the panel's driver disabled it on the way down, and it can come
						// back dark. Re-modeset on the fd we hold and re-arm the compositor. Closing it to reopen
						// would give up DRM master, and anything still holding the old file then leaves us locked
						// out of the panel (EACCES). A full reopen is only for a panel that will not restore.
						bool restored = false;
						for (int tries = 0; disp.isOpen() && !restored && tries < 50; tries++)
						{
							restored = disp.restore();
							if (!restored) sleepUs(20000);
						}
						if (restored)
						{
							comp.resume();
							// bananad's wake SIGCONTs the game's group: the pause menu keeps it frozen
							if (gamePid > 0 && pause.open) { signalTree(gamePid, SIGSTOP); pause.draw(); }
						}
						else if (gamePid > 0) logf("resume: panel restore failed (resident game)");
						else
						{
							logf("resume: panel restore failed, reopening it");
							comp.stop();
							disp.close();
							for (int tries = 0; !disp.openSingle(o.top, err) && tries < 50; tries++) sleepUs(20000);
							shaderApply(true);	// close() let go of the mask's buffers
							if (!comp.start(err)) logf("compositor: off: %s", err.c_str());
						}
						std::lock_guard<std::mutex> l(worker.uiLock);
						be.invalidate(); ui->dataChanged(); dirty = true; batteryDue = true; guard.wake(nowMs(), &sound); sound.setBgm(settings["bgm"] != "off"); sound.warm();
					}
					if (c == SIGUSR2)
					{
						// "volume 85" / "brightness 40", written just now (an old one is from a key pressed in a game)
						struct stat st;
						std::ifstream f("/run/banana/osd");
						std::string what; int pct = -1;
						if (stat("/run/banana/osd", &st) == 0 && time(nullptr) - st.st_mtime <= 2 && (f >> what >> pct) && pct >= 0)
						{
							osdLabel = what == "brightness" ? "Brightness" : "Volume";
							osdPercent = pct;
							osdUntil = nowMs() + 1500;
							dirty = true;
						}
					}
				}
			}
			else if (p.fd == powerFd)
			{
				if (powerUeventDrain(powerFd))
				{
					lib.forgetPower();	// the read below must see the new state, not the cached one
					batteryDue = true;
					dirty = true;
				}
			}
			else if (p.fd == roms.fd)
				roms.read(nowMs());
			else if (p.fd == launchWatch)
			{
				// The watch is /run/banana, for the launch file -- and for screen.req, the screen dump below, and the cheevos and
				// toast files, the pop-ups the loop reads on its next pass: one watch, nothing new polled.
				alignas(inotify_event) char buf[1024];
				ssize_t len;
				while ((len = read(launchWatch, buf, sizeof buf)) > 0)
				{
					for (char* e = buf; e + sizeof(inotify_event) <= buf + len; )
					{
						auto* ev = (inotify_event*)e;
						if (e + sizeof(inotify_event) + ev->len > buf + len) break;	// a short/corrupt read: don't walk past the buffer
						if (ev->len && !strcmp(ev->name, "screen.req")) shotWanted = true;
						else if (ev->len && !strcmp(ev->name, "cheevos")) cheevosSeen = true;
						else if (ev->len && !strcmp(ev->name, "toast")) cheevosSeen = true;
						else if (ev->len && !strcmp(ev->name, "overlay")) overlaySeen = true;
						e += sizeof(inotify_event) + ev->len;
					}
				}
				launchSeen = true;	// the loop's next pass reads the launch file
			}
			else if (p.fd == worker.wakeFd)
			{
				uint64_t v;
				if (read(worker.wakeFd, &v, 8) < 0) {}
				dirty = true;
			}
			else if (comp.wlFd() >= 0 && p.fd == comp.wlFd())
				comp.dispatch();
			else if (disp.isOpen() && p.fd == disp.pollFd())
			{
				disp.handleEvents();
				comp.vblank();	// a flip completed: release the client buffer, present the next frame
			}
			else
				input.read(p.fd, handle);
		}
		if (comp.eglActive())
		{
			static double lastEglBeat = 0;
			if (now - lastEglBeat >= 5000) { lastEglBeat = now; logf("compositor: EGL alive"); }
		}
		// the client went away (or its last frame completed): give the panel back to the menu with a full redraw
		bool gameNow = comp.gameActive() || comp.clientOnScreen();
		if (!gameNow) pause.close();	// the client went away while the quick menu was up
		if (gameWas && !gameNow) { be.invalidate(); dirty = true; drawn = false; }
		gameWas = gameNow;
		// A screen dump, asked for by writing /run/banana/screen.req (the launch watch above fires for it): the frame
		// the menu has composed, which is what a person looking at the handheld sees -- Display turns it for the panel
		// afterwards -- written as a JPEG to /run/banana/screen.jpg, with the request removed. For development over
		// the USB link (tools/rpc-screen.sh asks for one and fetches it).
		if (shotWanted)
		{
			shotWanted = false;
			const int w = be.width(), h = be.height();
			const uint32_t* px = be.pixels();
			std::vector<uint8_t> rgb((size_t)w * h * 3);
			for (size_t i = 0, pixelCount = (size_t)w * h; i < pixelCount; i++)
			{
				const uint32_t c = px[i];	// 0xAARRGGBB
				rgb[i * 3 + 0] = (uint8_t)(c >> 16);
				rgb[i * 3 + 1] = (uint8_t)(c >> 8);
				rgb[i * 3 + 2] = (uint8_t)c;
			}
			const char* tmp = "/run/banana/screen.jpg.tmp";
			if (stbi_write_jpg(tmp, w, h, 3, rgb.data(), 85) && !rename(tmp, "/run/banana/screen.jpg"))
				logf("screen: /run/banana/screen.jpg (%dx%d)", w, h);
			else
				logf("screen: could not write the screen dump");
			unlink("/run/banana/screen.req");
		}
	}
	{
		std::ofstream f(o.data + "/last-system");
		f << ui->systemIndex() << "\n";
	}
	worker.finish();
	return 0;
}
