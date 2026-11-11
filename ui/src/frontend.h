#pragma once
// banana-ui: what the menu draws from and draws with. A Source supplies systems, games and status; a Backend draws;
// the accent and a theme's own colours and files are shared by the menu and Settings.
#include "raster.h"
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rnds
{
	// ---- data ---------------------------------------------------------------------------------------------------------
	struct SysInfo
	{
		std::string id;			// icon/accent key
		std::string name;		// "Game Boy"
		std::string maker;		// "Nintendo"
		Color accent;
		std::string icon;		// 32x32 pixel icon (PNG path)
		int games = 0, played = 0;
		std::string time;		// "86h 12m"
		int achA = 0, achB = 0;	// RetroAchievements across its games: unlocked / total (achB 0: none known)
		std::string achLabel;	// "28 / 50 achievements" (empty when none known)
		bool fav = false;
		bool app = false;			// the system is one app (the theme's systems.cfg says "[app]"): A on its tile starts it
		bool hasResume = false;		// a game to show on the "last played" chip
		std::string resumeEyebrow;	// "Last played"
		std::string resumeTitle;
		std::string resumeBox;		// the chip's cartridge: box cover, the real cart scan, the label art (see GameInfo)
		std::string resumeCart, resumeLabel;
	};

	struct GameInfo
	{
		std::string title;
		std::string sub;		// bubble subtitle: "Role-playing · 2009"
		std::string plays;		// stats: "14", "Yesterday", "22h 10m"
		std::string last;
		std::string time;
		std::string box;		// the 2D box cover
		std::string cart;		// the real cartridge scan (its label art fills the drawn cartridge)
		std::string label;		// label art made from the cover (the media tool's "boxback"), when there is no scan
		std::string icon;		// what a cartridge with none of the art above shows on a plain label instead of
								// staying empty: a 32x32 pixel icon, its system's (a tool, an app, a game nobody scraped)
		int achA = 0, achB = 0;	// unlocked / total
		std::string achLabel;	// "28 / 50 achievements" (lower case; drawn upper)
		bool fav = false;
	};

	class Source
	{
	public:
		virtual ~Source() {}
		virtual int systemCount() = 0;
		virtual SysInfo system(int i) = 0;
		virtual int gameCount() = 0;			// the library's list (the current system)
		virtual GameInfo game(int i) = 0;
		virtual std::string clockText() = 0;	// "01:01 PM" / "13:01"
		virtual std::string dateText() = 0;		// "10/02"
		virtual float battery() = 0;			// 0..1, < 0 unknown
		virtual bool charging() { return false; }	// the charger is plugged in: the battery icon shows a bolt
		virtual std::string batteryMode() { return "icon"; }	// ES's "show battery status": "" none, "icon", "text" (icon and %)
		virtual bool topMotion() { return true; }	// false: the screen holds still (no rise on a move, no bob)
		virtual bool online() = 0;

		// actions
		virtual void selectSystem(int i) = 0;	// the home cursor moved
		virtual void selectGame(int i) = 0;		// the library cursor moved
		virtual int openLibrary(int sys) = 0;	// home -> library: the game index to start on
		virtual void backToHome() = 0;			// library -> home
		virtual void launch(int game) = 0;		// the ready screen's game (library index)
		virtual void toggleSystemFav(int sys) = 0;
		virtual void toggleGameFav(int game) = 0;
		virtual int resumeGame(int sys) = 0;	// library index of the system's resume game, opening that list; -1 none
		virtual void cue(const std::string& /*name*/) {}	// a theme sound for a moment of an animation ("insert")
		// the library with no games: what the bubble says ("No games yet") and the hint under it ("Copy .gb files to
		// roms/gb"); the host knows where the games go
		virtual std::string emptyTitle() { return "No games yet"; }
		virtual std::string emptyHint() { return ""; }
		// more buttons for the bottom bar, between its two (key, what it does), on the home shelf (library false: sys is
		// the system) or in a library (game: its index there); shown as long as they fit, from the first
		virtual std::vector<std::pair<std::string, std::string>> hints(bool /*library*/, int /*sys*/, int /*game*/) { return {}; }
	};

	// ---- drawing ------------------------------------------------------------------------------------------------------
	struct Tex
	{
		virtual ~Tex() {}
		int w = 0, h = 0;
		bool alpha = false;
	};
	typedef std::shared_ptr<Tex> TexPtr;

	class Backend
	{
	public:
		virtual ~Backend() {}
		virtual TexPtr upload(const Bitmap& b, bool smooth) = 0;
		// device pixels of the current screen; tint multiplies (alpha textures take its colour)
		virtual void draw(const TexPtr& t, float x, float y, float w, float h, const Color& tint) = 0;
		virtual void fill(float x, float y, float w, float h, const Color& c) = 0;
		virtual void pushClip(float x, float y, float w, float h) = 0;
		virtual void popClip() = 0;
		virtual void beginScreen() = 0;
		virtual void endScreen() = 0;
		// a post-effect over what is drawn so far in the box (device px): the mosaic, each block of `size` px the
		// colour of its top-left pixel. A backend without it draws nothing for it.
		virtual void mosaic(float x, float y, float w, float h, float size) { (void)x; (void)y; (void)w; (void)h; (void)size; }
	};

	// ---- the theme ----------------------------------------------------------------------------------------------------
	// the accent: the selected plate, the scroll thumb, held icons, the settings' pick, and every yellow of the logo
	// and the boot animation (setAccent before the UI is made; Banana's settings: yellow, blue, red, green, purple,
	// pink)
	extern std::string brandNote;	// beside the logo at the top (Banana: "Alpha 0.3.0"); empty: nothing
	struct Accent { uint32_t base, light, dark, band, low; };
	extern Accent accent;
	bool setAccent(const std::string& name);		// false: unknown, the yellow kept
	void setAccent(const Accent& a);				// a theme's own accent
	// a user theme's colours over the palette, by the palette's names ("ink", "screenBg", ...), 0xAARRGGBB; empty: none.
	// Applied when a palette is set (UI and Banana's settings screen alike)
	extern std::map<std::string, uint32_t> themeColors;
	// a user theme's own files (absolute paths, checked to exist; empty: the default) and switches
	struct ThemeFiles
	{
		std::string fontRegular, fontMedium;		// .ttf
		std::string wallTop;						// the picture behind the screen (scaled to fit)
		std::string logo;							// the logo at the top (scaled to the default's size)
		std::map<std::string, std::string> icons;	// system id -> .png
		std::map<std::string, std::string> sounds;	// scroll, select, back, fav, ready, insert -> .wav, or "off"
		bool dither = true;							// the screens' dot pattern
	};
	extern ThemeFiles themeFiles;
	bool themeColor(const char* name, Color& c);	// c replaced by the theme's colour of that name: true
	void tintAccent(Bitmap& b);						// RGBA8: its yellows in the accent's hue (nothing with the yellow)
	uint32_t tintAccent(uint32_t argb);				// one colour, 0xAARRGGBB (premultiplied is fine: hue and ratios)
}
