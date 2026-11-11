#pragma once
// banana-ui: the panel through KMS, with no compositor and no GPU. The panel gets two dumb scanout buffers;
// present() copies the part that changed into the hidden one and flips to it on the next vblank.
//
// Phase 3A: when the DRM driver exposes an overlay plane (and atomic KMS), the menu is drawn on that overlay
// (ARGB8888, premultiplied, zpos top) and a black buffer stays on the primary. The panel is portrait-native
// 1080x1240 while the menu draws 1240x1080: the rotation happens once, in software, in turn() (90 degrees
// clockwise). The Wayland output of a future game client is the logical 1240x1080; its game frame would be
// rotated by the same 90 degrees. Without an overlay/atomic KMS the old single-plane path is kept.
#include <cstdint>
#include <string>
#include <vector>

namespace banana
{
	// a rectangle of a screen that changed: [x0, x1) x [y0, y1)
	struct Damage
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool empty() const { return x0 >= x1 || y0 >= y1; }
		void add(const Damage& o)
		{
			if (o.empty()) return;
			if (empty()) { *this = o; return; }
			x0 = x0 < o.x0 ? x0 : o.x0; y0 = y0 < o.y0 ? y0 : o.y0;
			x1 = x1 > o.x1 ? x1 : o.x1; y1 = y1 > o.y1 ? y1 : o.y1;
		}
	};

	// One atomic request, filled by hand: drmModeAtomicAlloc allocates a request per commit, and commitFrame runs every
	// frame. The kernel reads count_objs object groups, each counts[i] properties taken in order from props and vals,
	// so properties are added object by object. Too many of either is an overflow: the frame is refused, never truncated.
	struct AtomicBuf
	{
		static constexpr int kMaxProps = 32, kMaxGroups = 8;
		uint32_t objs[kMaxGroups] = {}, counts[kMaxGroups] = {}, props[kMaxProps] = {};
		uint64_t vals[kMaxProps] = {};
		int n = 0, groups = 0;
		uint32_t last = 0;
		bool overflow = false;
		void clear() { n = groups = 0; last = 0; overflow = false; }
		void add(uint32_t obj, uint32_t prop, uint64_t v)
		{
			if (!prop) return;	// the plane has no such property: nothing to set
			if (n >= kMaxProps) { overflow = true; return; }
			if (groups && obj == last) counts[groups - 1]++;
			else if (groups < kMaxGroups) { objs[groups] = obj; counts[groups++] = 1; }
			else { overflow = true; return; }
			props[n] = prop; vals[n] = v; n++;
			last = obj;
		}
	};

	class Display
	{
	public:
		~Display() { close(); }
		// the panel: the RPC's single 1240x1080 screen. The connector named like top is used when it is connected,
		// else any connected connector with a mode (the type is only a preference here).
		bool openSingle(const std::string& top, std::string& err);
		void close();
		bool isOpen() const { return fd >= 0; }
		// how many panels present() drives: 1 on this handheld
		int panels() const { return count; }
		// how the panel is mounted, in degrees (the connector's "panel orientation": the RPC's panel is 1080x1240
		// portrait, its 90 in the device tree): present() turns the composed screen by it into the scanout buffers
		int rotation() const { return rot; }
		// the panel's own size (its scanout buffers'): 1080x1240 on the RPC while the menu draws 1240x1080
		int panelWidth() const { return SW; }
		int panelHeight() const { return SH; }
		int width() const { return W; }
		int height() const { return H; }
		int pollFd() const { return fd; }
		bool flipPending() const { return pending > 0; }
		// the composed screens (rows of width() words) and what changed on each since the last present
		bool present(const uint32_t* const screens[2], const Damage dmg[2]);
		void handleEvents();	// the flip events from pollFd()
		// after a resume or a lost master: show the current buffers again. False when another master still holds the
		// panel (a game's compositor letting go): the caller retries
		bool restore();
		// the night light: 0 off, 1 warm, 2 warmer. A gamma LUT on the panel, so it covers a game too (set from the
		// menu, which holds the master; the kernel keeps the LUT across the compositor hand-over)
		void setWarmth(int level);
		// Phase 3B: the DRM fd (the compositor imports a client's DMA-BUF on it), whether the atomic overlay path
		// is in use, and the two compositor frames. presentGame() puts a client's framebuffer on the primary and
		// hides the menu overlay; showMenu() puts the menu back. Both honor the one-flip-in-flight rule.
		int drmFd() const { return fd; }
		bool atomic() const { return count > 0 && p[0].atomic; }
		// overlayFb is the panel-sized ARGB card to show above the primary, 0 to hide the overlay
		bool presentGame(uint32_t fb, uint32_t w, uint32_t h, uint32_t overlayFb = 0, int fenceFd = -1);
		// 3F: the primary plane accepts IN_FENCE_FD (the EGL presenter may skip glFinish only when it does)
		bool fenceSupported() const { return count > 0 && p[0].primary.in_fence_fd != 0; }
		// the 3D card buffer: clear it, fill a panel-space rectangle (premultiplied ARGB), hand its FB to the
		// compositor. False when the atomic overlay path is not in use.
		bool overlayCardBegin();
		void overlayCardTurn(const uint32_t* canvas, int x0, int y0, int x1, int y1);
		uint32_t overlayCardFb() const;
		// a game's screen frame: canvas (rows of width() words, premultiplied ARGB) is turned into a hidden buffer and
		// becomes what frameFb() names, and the base every card is drawn on. nullptr takes it away. Not for a hot path:
		// it writes the whole panel. False when the atomic overlay path is not in use or no buffer could be made.
		bool frameSet(const uint32_t* canvas);
		uint32_t frameFb() const;
		bool showMenu();
		// black primary, overlay off: used before the compositor is torn down so the plane stops scanning a
		// presenter-owned framebuffer before the DRM fd is closed
		bool blank();

	private:
		struct Buf { uint32_t handle = 0, fb = 0, pitch = 0; uint64_t size = 0; uint8_t* map = nullptr; };
		// one DRM plane's ids and the properties this display needs (found by type and possible_crtcs, never by id)
		struct Plane
		{
			uint32_t id = 0, type = 0;
			uint32_t crtc_id = 0, fb_id = 0, src_x = 0, src_y = 0, src_w = 0, src_h = 0, crt_x = 0, crt_y = 0, crt_w = 0, crt_h = 0;
			uint32_t in_fence_fd = 0;	// 3F: the primary's GPU sync file property (0: unsupported)
			uint32_t zpos = 0, alpha = 0, blend = 0;
			uint64_t zpos_max = 0;
			bool zpos_mutable = false, has_alpha = false, has_blend = false;
		};
		struct Panel
		{
			uint32_t conn = 0, crtc = 0;
			uint8_t mode[68] = { 0 };
			Buf buf[2];		// the menu, ARGB8888 (atomic overlay) or XRGB8888 (legacy primary)
			Buf card[2];	// the 3D card layer, ARGB8888 over a client on the primary (atomic only)
			int cardFront = 0;
			Buf frame[2];	// a game's screen frame, made the first time one is shown
			int frameFront = 0;
			bool frameOn = false;
			Buf black;		// XRGB8888, the primary when the menu is on the overlay
			int front = 0;
			Damage missed[2];
			Plane primary, overlay;
			bool atomic = false;
			uint32_t mode_blob = 0;
		};
		int fd = -1, W = 0, H = 0, pending = 0, count = 1;
		AtomicBuf atomicBuf;	// the flip path's request, reused every frame
		int rot = 0, SW = 0, SH = 0;	// the panel's rotation, and its size in its own rows (the scanout buffers')
		Panel p[2];
		std::vector<uint8_t> frameCopy;	// the turned frame in cached memory: reading the uncached dumb buffer per card is slow
		void turn(const uint32_t* src, Buf& bf, const Damage& d);
		void applyGamma();
		int warmth = 0;
		bool makeBuf(Buf& b, uint32_t fourcc);
		void freeBuf(Buf& b);
		uint32_t propId(uint32_t obj, uint32_t type, const char* name, uint64_t* value = nullptr);
		bool findPlanes(Panel& pn, uint32_t crtc_index);
		bool atomicModeset(Panel& pn);
		bool commitFrame(Panel& pn, uint32_t primaryFb, uint32_t srcW, uint32_t srcH, uint32_t overlayFb, int fenceFd = -1);
	};
}
