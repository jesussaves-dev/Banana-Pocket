#pragma once
// Screen frames: the picture the compositor's overlay plane shows around a game, and the hole the game is placed in.
// Built on the CPU when a game starts or the pause menu's Frame row changes, never per frame. Kept apart from the
// display so it can be tested without a DRM device (ui/tests/frame-test.cpp). See
// docs/superpowers/specs/2026-11-10-compositor-bezels-design.md.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace banana
{
	enum class FrameKind { None, Art, Banana };

	// logical (1240x1080) pixels; w == 0 is no hole: the game has the whole panel
	struct FrameHole { int x = 0, y = 0, w = 0, h = 0; };

	// 0xRRGGBB, from the active theme
	struct FrameColors { uint32_t backdrop, dots, body, band, low, ring, accent; };

	// the word settings.conf and pergame.conf keep for a kind
	const char* frameKindWord(FrameKind k);

	// what a game shows: the master switch, then the game's own choice, then its system's, then the built-in default
	// (a system with art uses it, every other has none). "" and "auto" follow the next level; art on a system
	// without any falls back to the Banana frame.
	FrameKind resolveFrame(bool master, const std::string& game, const std::string& system, bool hasArt);

	// the Banana frame's hole for a system: the largest rectangle of its aspect inside the panel less the border
	FrameHole frameBananaHole(const std::string& systemId, int W, int H);

	// the bounding box of the fully transparent pixels of a W x H ARGB image; w == 0 when there are none
	FrameHole frameHoleOf(const uint32_t* argb, int W, int H);

	// a frame PNG as premultiplied ARGB, with its hole. False, with a reason, for a file that is not a W x H picture
	// or has no hole.
	bool frameLoadArt(const std::string& path, int W, int H, std::vector<uint32_t>& out, FrameHole& hole, std::string& err);

	// the Banana frame around `hole`, premultiplied ARGB: a raised band on the menu's dot-grid backdrop
	void frameDrawBanana(std::vector<uint32_t>& out, int W, int H, const FrameHole& hole, const FrameColors& c);

	// scale colour by percent and leave alpha alone: an opaque frame gets darker rather than see-through
	void frameDim(uint32_t* argb, size_t n, int percent);
}
