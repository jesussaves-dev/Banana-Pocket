#pragma once
// banana-ui: the pause menu. MENU over a game freezes it (SIGSTOP on the launcher's whole tree) and lists what can be
// done. The game's own state stays with the main loop, which launches and ends it; this holds references to it.
#include "compositor.h"
#include "frame.h"
#include "input.h"
#include "library.h"
#include "settings.h"
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <sys/types.h>
#include <vector>

namespace banana
{
	class PauseMenu
	{
	public:
		PauseMenu(Settings& settings, Library& lib, Input& input, Compositor& comp, pid_t& gamePid, double& gameStart, int& gameIndex, bool& gameRA,
		          bool& gameFramable, std::string& gameRom, FrameKind& frameKind, int& raSlot, int& discs, int& discNow, bool& ffOn,
		          std::deque<std::string>& raQueue)
			: settings(settings), lib(lib), input(input), comp(comp), gamePid(gamePid), gameStart(gameStart), gameIndex(gameIndex), gameRA(gameRA),
			  gameFramable(gameFramable), gameRom(gameRom), frameKind(frameKind), raSlot(raSlot), discs(discs), discNow(discNow),
			  ffOn(ffOn), raQueue(raQueue) {}

		bool open = false, pending = false;	// pending: MENU is held and may yet open it on release
		std::deque<int> keyQueue;			// evdev keys a chosen app row wants tapped in the client (sent by the main loop once it is thawed)
		Settings::PauseView view;			// what the overlay draws, rebuilt before each draw

		// set once by the main loop
		std::function<void()> drawCard, hideCard;
		std::function<FrameKind(const std::string&, FrameKind)> applyFrame;
		std::function<bool(const std::string&)> frameHasArt;
		std::function<bool()> frameMaster;

		void show();
		void close();
		void draw();
		void key(Key k);

	private:
		void build();
		void syncSlot();
		void addQuick();
		void act();

		Settings& settings;
		Library& lib;
		Input& input;
		Compositor& comp;
		pid_t& gamePid;
		double& gameStart;
		int& gameIndex;
		bool& gameRA;
		bool& gameFramable;
		std::string& gameRom;
		FrameKind& frameKind;
		int& raSlot;
		int& discs;
		int& discNow;
		bool& ffOn;
		std::deque<std::string>& raQueue;

		int sel = 0, armed = -1, pickSlot = 0, discPick = 1;
		int qBright = -1, qVol = -1;	// the quick settings' levels, read when the menu opens
		bool qWifi = false;
		std::vector<std::string> rows;	// the row ids (English labels) for this game
		std::vector<std::pair<std::string, int>> appKeys;	// an app's rows that tap a key: label, evdev code (from menu_keys)
	};
}
