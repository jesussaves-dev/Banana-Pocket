// banana-ui: the boot animation, and the iris from it into the menu.
#include "app.h"

// ---- the boot animation and the hand-over to the menu --------------------------------------------------------
// one frame to the panels, then wait for its flip (or a signal)
static void showFrame(Display& disp, SoftBackend& be, int sig)
{
	if (!disp.present(be.pixels(), be.takeDamage())) return;
	while (disp.flipPending())
	{
		pollfd p[2] = { { disp.pollFd(), POLLIN, 0 }, { sig, POLLIN, 0 } };
		if (poll(p, sig >= 0 ? 2 : 1, 100) <= 0 || (sig >= 0 && (p[1].revents & POLLIN))) return;
		disp.handleEvents();
	}
}

static Splash gSplash;
void forgetSplash() { gSplash = Splash(); }

// plays until the intro is over and the menu is ready (with the animation off: only the wait for the menu)
// The boot click's race: the splash fires it ~1 s after the panel opens, and the sound path (the card, WirePlumber's
// probe, the first stream starting the DSP) comes up at about the same moment -- a little later and the click was
// kept and played late, after the landing it belongs to. So the splash waits for the sound instead: it holds before
// the bunch starts to fall until a playback frontend is RUNNING (PipeWire has the DSP pulling audio, the amp locks
// within ~50 ms), then the fall's ~1 s leads into a click that plays the moment it is asked. Read-only: /proc/asound.
static bool audioFlowing()
{
	std::ifstream cards("/proc/asound/cards");
	std::string all((std::istreambuf_iterator<char>(cards)), std::istreambuf_iterator<char>());
	if (all.find('[') == std::string::npos)
	{
		// No card *yet* is the usual cold boot: the ADSP's APM only answers ~6.3 s after the kernel starts, and the
		// card binds after it -- later than the splash. Wait then; only a system with no DSP coming (QEMU) has nothing
		// to wait for.
		bool dsp = false;
		if (DIR* r = opendir("/sys/class/remoteproc"))
		{
			while (dirent* e = readdir(r)) if (strncmp(e->d_name, "remoteproc", 10) == 0) dsp = true;
			closedir(r);
		}
		return !dsp;
	}
	if (DIR* d = opendir("/proc/asound"))
	{
		bool running = false;
		while (dirent* e = readdir(d))
		{
			if (strncmp(e->d_name, "card", 4) != 0 || !isdigit((unsigned char)e->d_name[4])) continue;
			std::string card = std::string("/proc/asound/") + e->d_name;
			if (DIR* c = opendir(card.c_str()))
			{
				while (dirent* p = readdir(c))
				{
					size_t n = strlen(p->d_name);
					if (strncmp(p->d_name, "pcm", 3) != 0 || n < 5 || p->d_name[n - 1] != 'p') continue;
					std::ifstream st(card + "/" + p->d_name + "/sub0/status");
					std::string line;
					while (std::getline(st, line)) if (line.find("RUNNING") != std::string::npos) running = true;
				}
				closedir(c);
			}
		}
		closedir(d);
		return running;
	}
	return true;
}

void playSplash(Display& disp, SoftBackend& be, const Options& o, const std::string& variant, bool on,
	std::atomic<bool>& ready, std::mutex& readyM, std::condition_variable& readyCv, Sound& sound, int sig, bool waitAudio)
{
	if (!on || !gSplash.load(o.splash, o.banana + "/banana-48.png", variant, be.width(), be.height()))
	{
		// no animation: sleep until the loader publishes the menu instead of polling `ready` every 2 ms
		std::unique_lock<std::mutex> l(readyM);
		readyCv.wait(l, [&] { return ready.load(); });
		return;
	}
	gSplash.onCue = [&sound](const std::string& n) { sound.play(n); };
	double t0 = nowMs();
	// the hold (see audioFlowing): from the moment the clock reaches HOLD_AT until the sound plays, at most HOLD_MAX
	const double HOLD_MAX = 3500;	// measured holds up to ~2.2 s on a cold boot
	double holdStart = -1, offset = 0;
	bool released = !waitAudio;
	for (;;)
	{
		double raw = nowMs() - t0, t = raw - offset;
		if (!released && t >= Splash::HOLD_AT)
		{
			if (holdStart < 0) holdStart = raw - (t - Splash::HOLD_AT);
			bool flowing = audioFlowing();
			if (flowing || raw - holdStart >= HOLD_MAX)
			{
				released = true;
				offset = raw - holdStart;
				t = raw - offset;
				logf("splash: held %.0f ms for the sound%s", offset, flowing ? "" : " (gave up: not playing yet)");
			}
			else t = Splash::HOLD_AT;
		}
		bool r = ready;
		if (gSplash.finished(t, r)) break;
		gSplash.draw(be.canvas(), t, r);
		showFrame(disp, be, sig);
		int wait = gSplash.nextFrame(nowMs() - t0 - offset, ready);
		if (wait > 16) sleepUs(std::min(wait, 50) * 1000);	// waiting on the library: look again now and then
	}
}

// The iris: on each panel a circle round its centre, the picture inside it and black outside. Its edge goes in steps
// of the splash's own pixels (2 at 480 lines), as pixel art would draw it; timed by the clock, a frame each refresh.
struct Iris
{
	Display& disp;
	SoftBackend& be;
	int W, H, block;
	size_t n;
	float full;
	static constexpr double CLOSE = 360, HOLD = 70, OPEN = 450;
	Iris(Display& d, SoftBackend& b) : disp(d), be(b), W(b.width()), H(b.height())
	{
		n = (size_t)W * H;
		block = std::max(1, (int)std::lround(2 * H / 480.0f));
		full = std::sqrt((float)(W * W + H * H)) / 2 + block;	// the corners
	}
	void frame(const std::vector<uint32_t>& src, float r)
	{
		uint32_t* d = be.canvas();
		for (int y0 = 0; y0 < H; y0 += block)
		{
			// the row band's half-width inside the circle, in whole blocks
			float dy = (y0 + block * 0.5f) - H * 0.5f;
			float h2 = r * r - dy * dy;
			int half = h2 > 0 ? (int)(std::sqrt(h2) / block) * block : -1;
			int xa = half < 0 ? W : std::max(0, W / 2 - half), xb = half < 0 ? W : std::min(W, W / 2 + half);
			for (int y = y0; y < std::min(H, y0 + block); y++)
			{
				uint32_t* row = d + (size_t)y * W;
				const uint32_t* sr = src.data() + (size_t)y * W;
				if (xa > 0) std::fill(row, row + xa, 0xff000000u);
				if (xb > xa) std::copy(sr + xa, sr + xb, row + xa);
				if (xb < W) std::fill(row + std::max(xa, xb), row + W, 0xff000000u);
			}
		}
		be.invalidate();
		showFrame(disp, be, -1);
	}
	// black for a moment, then the circle opening on the menu: fast at first, settling. The menu as it will be 300 ms
	// on (before the opening ends, so its clock never goes back), opened on as a still picture: a live menu frame
	// (16-20 ms) each refresh made the opening miss refreshes
	void openOn(Menu& ui, double hold)
	{
		const double t1 = nowMs();
		std::vector<uint32_t> menu(n);
		ui.update(t1 + 300);
		ui.finishWork();
		be.invalidate();
		ui.render();
		std::copy(be.canvas(), be.canvas() + n, menu.begin());
		std::fill(be.canvas(), be.canvas() + n, 0xff000000u);	// black again
		be.invalidate();
		while (nowMs() - t1 < hold) showFrame(disp, be, -1);
		const double t2 = nowMs();
		for (;;)
		{
			double p = (nowMs() - t2) / OPEN;
			if (p >= 1) break;
			float e = (float)p; e = 1 - (1 - e) * (1 - e) * (1 - e);
			frame(menu, full * e);
		}
		std::copy(menu.begin(), menu.end(), be.canvas());
		be.invalidate();	// the menu's next frame is drawn in full
	}
};

// the splash hands over to the menu through the iris: closing over the splash's last frame to black, holding, then
// opening on the menu
void irisToMenu(Display& disp, SoftBackend& be, Menu& ui)
{
	if (!gSplash.played()) return;
	Iris iris(disp, be);
	std::vector<uint32_t> was(be.pixels(), be.pixels() + iris.n);
	// the menu's first frame drawn now, before the circle closes (its first draw takes ~130 ms: the glyphs and boxes
	// made; with the splash's last frame still and on screen that wait doesn't show), the splash's put back
	ui.update(nowMs());
	ui.finishWork();
	be.invalidate();
	ui.render();
	std::copy(was.begin(), was.end(), be.canvas());
	const double t0 = nowMs();
	for (;;)	// closing over the splash: slow at first, faster into the point (no lingering at a small circle)
	{
		double p = (nowMs() - t0) / Iris::CLOSE;
		if (p >= 1) break;
		iris.frame(was, iris.full * (float)(1 - p * p));
	}
	iris.frame(was, 0);
	iris.openOn(ui, Iris::HOLD);
}

