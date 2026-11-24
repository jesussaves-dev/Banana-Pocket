// Compiled as C++ to exercise the actual compositor with a fake Display and Presenter.
#include "../../../ui/src/compositor.cpp"

extern "C" {
#include "../src/video_wayland.h"
}

#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

namespace banana
{
namespace
{
	enum class Mode { Video, Unsupported, Lost };

	struct Test
	{
		Mode mode;
		unsigned renders = 0, flips = 0, menus = 0;
		bool acked = false, failCommit = true;
		std::string events;
	};

	Test* test;

	class TestPresenter : public Presenter
	{
	public:
		const char* name() const override { return "video fixture"; }
		bool rotated() const override { return true; }
		bool acceptsShm() const override { return test->mode != Mode::Unsupported; }
		bool start(std::string&) override { return true; }
		void stop() override {}
		void presented() override {}
		void abandon() override {}
		void flipDone() override {}
		void detached() override {}
		void forget(ClientBuffer&) override {}
		bool render(ClientBuffer& b, uint32_t& fb, std::string&) override
		{
			assert(test->acked && b.shm && b.nativeBusy && !b.upright && b.shmDirty);
			assert(b.w == 2 && b.h == (test->renders < 2 ? 2u : 1u));
			assert(b.stride == 8);
			assert(b.nativeGeometry.rotation == test->renders);
			if (test->renders < 2)
				assert(b.nativeGeometry.width == 2 && b.nativeGeometry.height == 2);
			else
				assert(b.nativeGeometry.width == (1u << 24) && b.nativeGeometry.height == (1u << 23));
			wl_shm_buffer_begin_access(b.shm);
			auto* pixels = static_cast<const uint32_t*>(wl_shm_buffer_get_data(b.shm));
			if (test->renders == 0)
			{
				assert((pixels[0] & 0xffffff) == 0x123456 && (pixels[1] & 0xffffff) == 0x102030);
				assert((pixels[2] & 0xffffff) == 0x456789 && (pixels[3] & 0xffffff) == 0x987654);
			}
			else if (test->renders == 1)
			{
				assert(pixels[0] == 0xff0000 && pixels[1] == 0x00ff00);
				assert(pixels[2] == 0x0000ff && pixels[3] == 0x848284);
			}
			else
				assert(pixels[0] == 0x0000ff && pixels[1] == 0x848484);
			wl_shm_buffer_end_access(b.shm);
			b.shmDirty = false;
			fb = 70 + test->renders++;
			return true;
		}
	};

	void protocolLog(void*, wl_protocol_logger_type kind, const wl_protocol_logger_message* message)
	{
		std::string_view iface = wl_resource_get_class(message->resource);
		std::string_view name = message->message->name;
		if (kind == WL_PROTOCOL_LOGGER_REQUEST)
		{
			if (iface == "xdg_surface" && name == "ack_configure") test->acked = true;
			return;
		}
		if (iface == "banana_retro_surface_v1")
		{
			if (name == "first_presented")
			{
				assert(test->flips == 1 && test->mode == Mode::Video);
				test->events += 'P';
			}
			else if (name == "frame_failed") test->events += 'F';
			else if (name == "closed") test->events += 'X';
		}
		else if (iface == "wl_buffer" && name == "release") test->events += 'R';
		else if (iface == "wl_callback" && name == "done" && test->renders) test->events += 'D';
	}

	void checkGeometry()
	{
		uint32_t w = 320, h = 240;
		NativeGeometry{}.displayAspect(w, h);
		assert(w == 320 && h == 240);

		const float expectedUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		for (uint32_t rotation = 0; rotation < 4; ++rotation)
		{
			NativeGeometry geometry{4, 3, rotation};
			geometry.displayAspect(w, h);
			uint32_t x, y, fittedW, fittedH;
			fitRotatedHole(w, h, 1240, 1080, 10, 20, 240, 240, x, y, fittedW, fittedH);
			if (rotation & 1)
				assert(x == 820 && y == 40 && fittedW == 240 && fittedH == 180);
			else
				assert(x == 850 && y == 10 && fittedW == 180 && fittedH == 240);
			const float* uv = geometry.uvRotation();
			assert(0.5f - 0.5f * (uv[0] + uv[2]) == expectedUv[rotation][0]);
			assert(0.5f - 0.5f * (uv[1] + uv[3]) == expectedUv[rotation][1]);
		}
	}

	void runClient(Mode mode, int socket)
	{
		char fd[32];
		snprintf(fd, sizeof fd, "%d", socket);
		assert(setenv("WAYLAND_SOCKET", fd, 1) == 0);
		auto* v = wayland_video_open();
		if (mode == Mode::Unsupported)
		{
			assert(!v);
			return;
		}
		assert(v && !wayland_video_presented(v));
		retro_game_geometry tooBig = {2, 2, 4096, 4096, 1};
		assert(!wayland_video_configure(v, &tooBig));
		retro_game_geometry g = {2, 2, 4, 4, 0};
		assert(wayland_video_configure(v, &g));

		uint32_t rgb8888[] = {0x123456, 0x102030, 0, 0x456789, 0x987654, 0};
		assert(!wayland_video_submit(v, rgb8888, 0, 2, 12, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		assert(!wayland_video_submit(v, rgb8888, 2, 2, 7, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		assert(!wayland_video_submit(v, rgb8888, 2, 2, SIZE_MAX, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		assert(!wayland_video_submit(v, rgb8888, 5, 1, 20, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		assert(wayland_video_submit(v, nullptr, 0, 0, 0, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));

		assert(wayland_video_submit(v, rgb8888, 2, 2, 12, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		if (mode == Mode::Lost)
		{
			assert(!wayland_video_wait(v) && !wayland_video_presented(v));
			wayland_video_close(v);
			return;
		}
		assert(wayland_video_wait(v) && !wayland_video_presented(v));

		uint16_t rgb565[] = {0xf800, 0x07e0, 0, 0x001f, 0x8410, 0};
		assert(wayland_video_submit(v, rgb565, 2, 2, 6, RETRO_PIXEL_FORMAT_RGB565, &g, 1));
		assert(wayland_video_wait(v) && wayland_video_presented(v));

		g.aspect_ratio = 2.0f;
		uint16_t rgb1555[] = {0x001f, 0xc210};
		assert(wayland_video_submit(v, rgb1555, 2, 1, 4, RETRO_PIXEL_FORMAT_0RGB1555, &g, 2));
		// Both buffers are owned by the compositor until the replacement flips.
		assert(wayland_video_submit(v, rgb8888, 2, 2, 12, RETRO_PIXEL_FORMAT_XRGB8888, &g, 0));
		assert(wayland_video_wait(v));
		assert(wayland_video_submit(v, nullptr, 0, 0, 0, RETRO_PIXEL_FORMAT_RGB565, &g, 3));
		assert(wayland_video_wait(v));
		wayland_video_close(v);
	}
}

	void Display::close() {}
	bool Display::restore()
	{
		fd = 0;
		SW = 1080;
		SH = 1240;
		return true;
	}
	void Display::handleEvents()
	{
		assert(flipPending());
		--pending;
	}
	bool Display::showMenu()
	{
		assert(!flipPending());
		++test->menus;
		++pending;
		return true;
	}
	bool Display::presentGame(uint32_t fb, uint32_t w, uint32_t h, uint32_t overlay, int fence,
	                          uint32_t x, uint32_t y, uint32_t dw, uint32_t dh)
	{
		assert(!flipPending() && fb >= 70 && w == 1080 && h == 1240);
		assert(overlay == 42 && fence == -1 && x == 0 && y == 0 && dw == 0 && dh == 0);
		if (test->failCommit)
		{
			test->failCommit = false;
			return false;
		}
		++pending;
		return true;
	}

	void runTest(Mode mode)
	{
		int sockets[2];
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
		pid_t pid = fork();
		assert(pid >= 0);
		if (pid == 0)
		{
			close(sockets[0]);
			runClient(mode, sockets[1]);
			_exit(0);
		}
		close(sockets[1]);

		Test state = {mode, 0, 0, 0, false, mode != Mode::Lost, {}};
		test = &state;
		Display display;
		assert(display.restore());
		Compositor::Impl im;
		im.d = &display;
		im.overlayFb = 42;
		im.presenter = std::make_unique<TestPresenter>();
		im.wl = wl_display_create();
		assert(im.wl);
		wl_list_init(&im.pendCbs);
		wl_list_init(&im.pendingCbs);
		wl_list_init(&im.inflightCbs);

		assert(wl_display_init_shm(im.wl) == 0);
		assert(wl_global_create(im.wl, &wl_compositor_interface, 4, &im, compositorBind));
		assert(wl_global_create(im.wl, &xdg_wm_base_interface, 1, &im, xdgBind));
		assert(wl_global_create(im.wl, &banana_retro_manager_v1_interface, 1, &im, nativeBind));
		int probePeer = -1;
		if (mode == Mode::Video)
		{
			int probe[2];
			assert(socketpair(AF_UNIX, SOCK_STREAM, 0, probe) == 0);
			probePeer = probe[1];
			im.client = wl_client_create(im.wl, probe[0]);
			errno = 0;
			im.child = wl_client_create(im.wl, sockets[0]);
			int createError = errno;
			if (!im.child)
			{
				errno = 0;
				int fdFlags = fcntl(sockets[0], F_GETFD);
				int fdError = errno;
				errno = 0;
				int fileFlags = fcntl(sockets[0], F_GETFL);
				int fileError = errno;
				int socketError = -1, socketType = -1;
				socklen_t optionLength = sizeof(socketError), typeLength = sizeof(socketType);
				errno = 0;
				int socketErrorResult = getsockopt(sockets[0], SOL_SOCKET, SO_ERROR, &socketError, &optionLength);
				int socketErrorErrno = errno;
				errno = 0;
				int socketTypeResult = getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, &socketType, &typeLength);
				int socketTypeErrno = errno;
				socklen_t peerLength = sizeof(struct sockaddr_un);
				struct sockaddr_un peer;
				errno = 0;
				int peerResult = getpeername(sockets[0], (struct sockaddr*)&peer, &peerLength);
				int peerError = errno;
				int childStatus = 0;
				errno = 0;
				pid_t childWait = waitpid(pid, &childStatus, WNOHANG);
				int waitError = errno;
				fprintf(stderr,
				        "Wayland fixture child create failed: errno=%d (%s), fd=%d fdFlags=%d fdErr=%d "
				        "fileFlags=%d fileErr=%d soError=%d soErrorResult=%d soErrorErr=%d "
				        "type=%d typeResult=%d typeErr=%d peerResult=%d peerErr=%d childPid=%ld "
				        "waitPid=%ld waitErr=%d childStatus=%d "
				        "order=socketpair,fork,parent-close-child-end,probe-client,child-client\n",
				        createError, strerror(createError), sockets[0], fdFlags, fdError, fileFlags, fileError,
				        socketError, socketErrorResult, socketErrorErrno, socketType, socketTypeResult,
				        socketTypeErrno, peerResult, peerError, (long)pid, (long)childWait, waitError,
				        childStatus);
			}
			assert(im.child);
			im.childGone.notify = onChildGone;
			wl_client_add_destroy_listener(im.child, &im.childGone);
		}
		else im.client = wl_client_create(im.wl, sockets[0]);
		if (!im.client) perror("Creating fixture Wayland client");
		assert(im.client);
		im.clientGone.notify = onClientGone;
		wl_client_add_destroy_listener(im.client, &im.clientGone);
		auto* logger = wl_display_add_protocol_logger(im.wl, protocolLog, nullptr);
		assert(logger);

		auto* loop = wl_display_get_event_loop(im.wl);
		while (im.client)
		{
			assert(wl_event_loop_dispatch(loop, 3000) == 0);
			if (probePeer >= 0 && !im.surfaces.empty() && im.surfaces.front()->waitingConfigure)
			{
				assert(!im.surfaces.front()->configured && im.child);
				close(probePeer);
				probePeer = -1;
			}
			if (display.flipPending() && im.inflight)
			{
				// A render and accepted atomic commit cannot acknowledge first presentation.
				size_t events = state.events.size();
				im.vblank();
				assert(state.events.size() == events);
				if (mode == Mode::Lost && im.inflightNative)
					im.closeNative(im.inflightNative, true);
				++state.flips;
			}
			if (display.flipPending())
			{
				display.handleEvents();
				im.vblank();
			}
			wl_display_flush_clients(im.wl);
		}
		if (display.flipPending())
		{
			display.handleEvents();
			im.vblank();
		}
		int status = 0;
		assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
		assert(probePeer == -1 && !im.child && !im.inflight && !im.pending);
		assert(!im.held && !im.retired && im.surfaces.empty());
		if (mode == Mode::Video)
		{
			// Reject, release, done. Real flip, acknowledge first, done. Replacements
			// release the previous slot before the callback. The client destroys the
			// final buffer before the menu flip, so no release event is sent for it.
			if (state.events != "FRDPDRDRD") fprintf(stderr, "Video events: %s\n", state.events.c_str());
			assert(state.events == "FRDPDRDRD");
			assert(state.renders == 4 && state.flips == 3 && state.menus == 1);
		}
		else if (mode == Mode::Lost)
		{
			if (state.events != "XD") fprintf(stderr, "Surface loss events: %s\n", state.events.c_str());
			assert(state.events == "XD" && state.renders == 1 && state.flips == 1 && state.menus == 1);
		}
		else
			assert(state.events.empty() && state.renders == 0 && state.flips == 0 && state.menus == 0);
		wl_protocol_logger_destroy(logger);
		wl_display_destroy(im.wl);
	}
}

int main()
{
	alarm(15);
	banana::checkGeometry();
	banana::runTest(banana::Mode::Video);
	banana::runTest(banana::Mode::Unsupported);
	banana::runTest(banana::Mode::Lost);
	puts("banana-retro Wayland video checks passed");
}
