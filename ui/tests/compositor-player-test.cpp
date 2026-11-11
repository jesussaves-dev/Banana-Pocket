// BananaTube's player under the in-process compositor: real mpv processes (--vo=wlshm) against the real Wayland server,
// with the KMS and EGL halves faked, so what is checked is what a stack of clients does. mpv is its own process, so
// it arrives as a second client over the app; before the compositor took one, it was refused and played sound only.
//   compositor-player-test <wayland-dir>        (the dir compositor.cpp was built with)
#define private public
#include "../src/kms.h"
#undef private
#include "../src/client_buffer.h"
#include "../src/compositor.h"
#include "../src/egl_presenter.h"
#include <wayland-server-core.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace banana;

static char seen = '?';		// the colour of the last frame the presenter drew: r, b, g
static int rendered = 0, menus = 0;

// ---- the faked halves ---------------------------------------------------------------------------------------------
namespace banana
{
	bool Display::presentGame(uint32_t, uint32_t, uint32_t, uint32_t, int) { pending = 1; return true; }
	bool Display::showMenu() { menus++; return true; }
	void Display::handleEvents() { pending = 0; }
	bool Display::blank() { return true; }
	void Display::close() { fd = -1; }

	struct EglPresenter::Impl {};
	EglPresenter::EglPresenter(Display&) {}
	EglPresenter::~EglPresenter() {}
	bool EglPresenter::start(std::string&) { return true; }
	void EglPresenter::stop() {}
	bool EglPresenter::acceptsShm() const { return true; }
	bool EglPresenter::scanning() const { return false; }
	void EglPresenter::quiesce(bool) {}
	void EglPresenter::setHole(int, int, int, int) {}
	int EglPresenter::takeFenceFd() { return -1; }
	void EglPresenter::presented() {}
	void EglPresenter::abandon() {}
	void EglPresenter::flipDone() {}
	void EglPresenter::detached() {}
	void EglPresenter::forget(ClientBuffer&) {}
	bool EglPresenter::render(ClientBuffer& b, uint32_t& fb, std::string& err)
	{
		if (b.shmDirty)
		{
			if (!b.shm) { err = "gone"; return false; }
			wl_shm_buffer_begin_access(b.shm);
			const auto* px = (const uint8_t*)wl_shm_buffer_get_data(b.shm) + (size_t)(b.h / 2) * b.stride + (b.w / 2) * 4;
			seen = px[2] > 150 && px[0] < 100 ? 'r' : px[0] > 150 && px[2] < 100 ? 'b' : px[1] > 100 ? 'g' : 'k';
			wl_shm_buffer_end_access(b.shm);
			b.shmDirty = false;
		}
		rendered++;
		fb = 1;
		return true;
	}
	bool DirectPresenter::start(std::string&) { return true; }
	bool DirectPresenter::render(ClientBuffer&, uint32_t&, std::string&) { return false; }
	void DirectPresenter::forget(ClientBuffer&) {}
}

// ---- the test -----------------------------------------------------------------------------------------------------
static Compositor* comp;
static int fails;
static void check(bool ok, const char* what) { printf("  %s: %s\n", ok ? "ok" : "FAIL", what); if (!ok) fails++; }
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
// the menu's loop in small: the server's fd, and a vblank every 16 ms
static void pump(double seconds, char until = 0)
{
	double end = now() + seconds;
	while (now() < end && !(until && seen == until))
	{
		pollfd p = { comp->wlFd(), POLLIN, 0 };
		poll(&p, 1, 16);
		comp->dispatch();
		comp->display().pending = 0;
		comp->vblank();
		comp->flush();
	}
}
// The app stands in for BananaTube's SDL window: an ordinary client, red until SIGUSR1 asks it to repaint green.
static volatile sig_atomic_t repaint;
static wl_compositor* wc; static wl_shm* wshm; static xdg_wm_base* wm;
static void regGlobal(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t)
{
	if (!strcmp(iface, "wl_compositor")) wc = (wl_compositor*)wl_registry_bind(r, name, &wl_compositor_interface, 4);
	if (!strcmp(iface, "wl_shm")) wshm = (wl_shm*)wl_registry_bind(r, name, &wl_shm_interface, 1);
	if (!strcmp(iface, "xdg_wm_base")) wm = (xdg_wm_base*)wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	if (!strcmp(iface, "wp_viewporter")) _exit(3);	// the kiosk client must not be offered it
}
static void regGone(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener regL = { regGlobal, regGone };
static bool acked;
static void xsConf(void*, xdg_surface* x, uint32_t serial) { xdg_surface_ack_configure(x, serial); acked = true; }
static const xdg_surface_listener xsL = { xsConf };
static void onUsr1(int) { repaint = 1; }
static void appMain(const char* dir)
{
	signal(SIGUSR1, onUsr1);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	wl_display* dpy = wl_display_connect("banana-0");
	if (!dpy) _exit(2);
	wl_registry_add_listener(wl_display_get_registry(dpy), &regL, nullptr);
	wl_display_roundtrip(dpy);
	if (!wc || !wshm || !wm) _exit(2);
	wl_surface* s = wl_compositor_create_surface(wc);
	xdg_surface* xs = xdg_wm_base_get_xdg_surface(wm, s);
	xdg_surface_add_listener(xs, &xsL, nullptr);
	xdg_surface_get_toplevel(xs);
	wl_surface_commit(s);
	while (!acked && wl_display_dispatch(dpy) >= 0) {}
	const int w = 64, h = 64, sz = w * h * 4;
	int fd = memfd_create("app", 0);
	if (fd < 0 || ftruncate(fd, sz * 2)) _exit(2);
	auto* px = (uint32_t*)mmap(nullptr, sz * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	wl_shm_pool* pool = wl_shm_create_pool(wshm, fd, sz * 2);
	wl_buffer* red = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_buffer* green = wl_shm_pool_create_buffer(pool, sz, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	for (int i = 0; i < w * h; i++) { px[i] = 0xffff0000; px[w * h + i] = 0xff00ff00; }
	wl_surface_attach(s, red, 0, 0);
	wl_surface_commit(s);
	wl_display_flush(dpy);
	for (;;)
	{
		pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
		poll(&p, 1, 20);
		if (p.revents && wl_display_dispatch(dpy) < 0) _exit(0);
		if (repaint) { repaint = 0; wl_surface_attach(s, green, 0, 0); wl_surface_commit(s); wl_display_flush(dpy); }
	}
}
static pid_t mpv(const char* dir, const char* colour)
{
	pid_t p = fork();
	if (p) return p;
	setenv("XDG_RUNTIME_DIR", dir, 1);
	setenv("WAYLAND_DISPLAY", "banana-0", 1);
	std::string src = std::string("av://lavfi:color=c=") + colour + ":s=320x240:r=30";
	execlp("mpv", "mpv", "--no-config", "--no-terminal", "--vo=wlshm", "--fullscreen", "--force-window=yes", "--no-audio",
	       "--loop=inf", src.c_str(), (char*)nullptr);
	_exit(127);
}
static void reap(pid_t p) { kill(p, SIGKILL); waitpid(p, nullptr, 0); }

int main(int argc, char** argv)
{
	if (argc < 2) return 2;
	signal(SIGPIPE, SIG_IGN);
	Compositor c;
	comp = &c;
	Display& d = c.display();
	d.fd = 99;
	d.panel.atomic = true;
	d.SW = 1080; d.SH = 1240;
	std::string err;
	if (!c.start(err)) { printf("  FAIL: start: %s\n", err.c_str()); return 1; }

	pid_t app = fork();
	if (!app) appMain(argv[1]);
	pump(5, 'r');
	check(seen == 'r', "the app's shm frame reaches the presenter");

	pid_t player = mpv(argv[1], "blue");
	pump(15, 'b');
	check(seen == 'b', "the player (real mpv) covers the app");
	int before = rendered;
	pump(1);
	check(rendered > before + 5, "the player's frames keep coming (frame callbacks and releases work)");

	pid_t third = mpv(argv[1], "yellow");
	pump(3);
	check(seen == 'b', "a third client is refused");
	reap(third);

	// the app repaints before the player's hangup is heard: that frame must not be lost
	kill(app, SIGUSR1);
	pump(1);
	check(seen == 'b', "a frame the app draws under the player stays hidden");
	reap(player);
	pump(5, 'g');
	check(seen == 'g', "the app's newest frame comes back when the player leaves");
	check(menus == 0, "the menu stays away while the app is up");

	player = mpv(argv[1], "blue");
	pump(15, 'b');
	check(seen == 'b', "a new player covers again (a quality reload)");
	reap(app);
	pump(2);
	check(menus == 1, "the app gone: the player goes with it and the menu returns");
	reap(player);
	c.stop();
	printf("%d failed\n", fails);
	return fails ? 1 : 0;
}
