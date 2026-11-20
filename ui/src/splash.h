#pragma once
// banana-ui: the boot animation, from the first frame while the library is read on another thread.
// The GBA BIOS start-up screen, in Banana's branding: a warm 240x160 scene (integer-scaled to the panel), the
// word BANANA flying in letter by letter from the bottom-right, POCKET settling in under it, a colour chase that settles to the banana tones,
// a sparkle sweep, and the pixel bunch where the Nintendo wordmark sits. It holds the seated word until the menu
// is ready; the iris in boot_anim.cpp takes over from there.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace banana
{
	class Splash
	{
	public:
		// dir: the splash assets (word.png); banana: the pixel bunch (assets/banana/banana-32.png); variant: dark or
		// light (unused: the splash is always white)
		bool load(const std::string& dir, const std::string& banana, const std::string& variant, int w, int h);
		// the screen at t ms after the start; ready: the menu is ready to take over (starts the fade of the last frame to white: from the moment it is seen, or the intro's end if later)
		void draw(uint32_t* scr, double t, bool ready);
		// the intro has played to its end and the menu is ready: time to hand over
		bool finished(double t, bool ready) const { return ready && fadeT >= 0 && t >= fadeT + FADE_MS; }
		void rewind() { fadeT = -1; cued = false; }	// start the clock again (the headless shots draw out of order)
		bool played() const { return W > 0; }
		// ms until the next frame is different (16 while anything moves); -1: nothing changes until the menu is ready
		int nextFrame(double t, bool ready) const;
		std::function<void(const std::string&)> onCue;	// "boot": once, when the first letter starts to move (the clip is scored to the intro)

		static constexpr double INTRO_END = 3750;
		static constexpr double FADE_MS = 250;
		// the frame before the first letter moves (only the white screen and the bunch): the host may hold the
		// animation here until the sound is actually playing, so the landing and its click always arrive together
		static constexpr double HOLD_AT = 60;

	private:
		struct Rgba { int w = 0, h = 0; std::vector<uint32_t> px; };	// premultiplied 0xAARRGGBB
		struct Mask { int w = 0, h = 0; std::vector<uint8_t> a; std::vector<int> letters, ends; };
		int W = 0, H = 0, KW = 1, g = 1, ox = 0, oy = 0;	// panel, device px per mask px, scene scale, origin
		bool cued = false;
		double fadeT = -1;	// when the fade to white began (-1: not yet)
		Rgba bunch;
		Mask word, pocket;
		int KP = 1;	// device px per mask px for POCKET (half BANANA's height)

		int VY(float v) const;
		void fill(uint32_t* d, int x, int y, int w, int h, uint32_t c) const;
		void background(uint32_t* d) const;
		void drawPocket(uint32_t* d, double t, double seat) const;
		void drawBunch(uint32_t* d, int x, int y, int k, int alpha) const;
		void drawLetter(uint32_t* d, int c0, int c1, double cx, double cy, double k, double t, double mix, int i) const;
	};
}
