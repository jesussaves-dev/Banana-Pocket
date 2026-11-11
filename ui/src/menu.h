#pragma once
// banana-ui: the menu of the theme in use, as the main loop drives it (LayoutMenu).
#include "frontend.h"
#include <functional>
#include <string>

namespace banana
{
	class Menu
	{
	public:
		enum View { HOME, LIBRARY, LAUNCH };
		enum Button { LEFT, RIGHT, UP, DOWN, A, B, X, Y, L, R };
		virtual ~Menu() {}
		virtual void setHome(int sys) = 0;
		virtual void setLibrary(int sys, int game) = 0;
		virtual void dataChanged() = 0;
		virtual View view() const = 0;
		virtual int systemIndex() const = 0;
		virtual int gameIndex() const = 0;
		virtual bool press(Button b) = 0;
		virtual void jumpTo(int game) = 0;	// the library's cursor straight to a game (the shoulder-button letter jump)
		virtual bool touch(float x, float y, bool down) = 0;
		virtual bool touchMove(float x, float y) = 0;
		virtual void touchCancel() = 0;
		virtual void update(double nowMs) = 0;
		virtual void render() = 0;
		virtual int nextWake() const = 0;
		virtual bool workOnce() = 0;
		virtual void finishWork() = 0;
		virtual void setNotifiers(std::function<void()> queued, std::function<void()> finished) = 0;
		virtual bool animating() const = 0;
		virtual void setLog(std::function<void(const std::string&)> log) = 0;
		// workOnce guards what it shares with the drawing itself: the worker may run it without the UI's lock
		virtual bool ownLocking() const { return false; }
	};
}
