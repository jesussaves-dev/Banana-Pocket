// The screen frames (src/frame.cpp) on the host: which frame a game gets, where its hole is, the shipped art.
//   frame-test <overlays dir> [out.png]    out.png: the Banana frame, for a look
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"
#include <cstdio>
#include "../src/frame.h"

using namespace banana;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static bool same(const FrameHole& h, int x, int y, int w, int hh) { return h.x == x && h.y == y && h.w == w && h.h == hh; }

int main(int argc, char** argv)
{
	const int W = 1240, H = 1080;
	// resolution: master, then the game, then the system, then the built-in default
	CHECK(resolveFrame(false, "banana", "art", true) == FrameKind::None);
	CHECK(resolveFrame(true, "", "", true) == FrameKind::Art);
	CHECK(resolveFrame(true, "", "", false) == FrameKind::None);
	CHECK(resolveFrame(true, "", "auto", true) == FrameKind::Art);
	CHECK(resolveFrame(true, "", "banana", true) == FrameKind::Banana);
	CHECK(resolveFrame(true, "none", "banana", true) == FrameKind::None);
	CHECK(resolveFrame(true, "banana", "none", false) == FrameKind::Banana);
	CHECK(resolveFrame(true, "auto", "none", true) == FrameKind::None);
	CHECK(resolveFrame(true, "art", "", false) == FrameKind::Banana);	// no art for this system
	CHECK(resolveFrame(true, "nonsense", "", true) == FrameKind::None);

	// the Banana hole: the panel less 32 px a side, at the system's aspect
	CHECK(same(frameBananaHole("snes", W, H), 32, 99, 1176, 882));
	CHECK(same(frameBananaHole("gba", W, H), 32, 148, 1176, 784));
	CHECK(same(frameBananaHole("ports", W, H), 32, 32, 1176, 1016));
	FrameHole gb = frameBananaHole("gb", W, H);
	CHECK(gb.h == 1016 && gb.w == 1128 && gb.w % 2 == 0);

	// a hole needs real size: stray transparent pixels are not one
	{
		std::vector<uint32_t> px((size_t)W * H, 0xff000000u);
		px[5] = 0;
		CHECK(frameHoleOf(px.data(), W, H).w == 0);
	}

	// the shipped art, with its grid gone: exactly the window each frame was cut for
	if (argc > 1)
	{
		struct { const char* n; int x, y, w, h; } art[] = {
			{ "gb.png", 60, 36, 1120, 1008 }, { "gbc.png", 60, 36, 1120, 1008 }, { "gba.png", 20, 140, 1200, 800 },
		};
		for (auto& a : art)
		{
			std::vector<uint32_t> px;
			FrameHole h;
			std::string err;
			bool ok = frameLoadArt(std::string(argv[1]) + "/" + a.n, W, H, px, h, err);
			if (!ok) fprintf(stderr, "%s\n", err.c_str());
			CHECK(ok);
			CHECK(same(h, a.x, a.y, a.w, a.h));
			size_t solid = 0;
			for (int y = a.y; y < a.y + a.h; y++)
				for (int x = a.x; x < a.x + a.w; x++) solid += px[(size_t)y * W + x] != 0;
			CHECK(solid == 0);
		}
		std::vector<uint32_t> px;
		FrameHole h;
		std::string err;
		CHECK(!frameLoadArt(std::string(argv[1]) + "/none.png", W, H, px, h, err));
	}

	// the Banana frame: the hole is clear, everything else is opaque, and it is found again by its own alpha
	FrameColors col = { 0x0b0a0d, 0x2e2b33, 0x17161c, 0x3a3540, 0x0b0a0d, 0x000000, 0xffd23f };
	std::vector<uint32_t> px;
	FrameHole snes = frameBananaHole("snes", W, H);
	frameDrawBanana(px, W, H, snes, col);
	CHECK(px.size() == (size_t)W * H);
	FrameHole found = frameHoleOf(px.data(), W, H);
	CHECK(same(found, snes.x, snes.y, snes.w, snes.h));
	size_t clear = 0;
	for (uint32_t v : px) clear += !(v >> 24);
	CHECK(clear == (size_t)snes.w * snes.h);

	// brightness scales colour, never alpha, so a dimmed frame still hides what is behind it
	{
		uint32_t v[3] = { 0xffc86432u, 0x00000000u, 0x80402010u };
		frameDim(v, 3, 50);
		CHECK(v[0] == 0xff643219u);
		CHECK(v[1] == 0);
		CHECK(v[2] >> 24 == 0x80);
		uint32_t w = 0xff123456u;
		frameDim(&w, 1, 100);
		CHECK(w == 0xff123456u);
	}

	if (argc > 2)
	{
		std::vector<unsigned char> rgba(px.size() * 4);
		for (size_t i = 0; i < px.size(); i++)
		{
			rgba[i * 4] = px[i] >> 16; rgba[i * 4 + 1] = px[i] >> 8; rgba[i * 4 + 2] = px[i]; rgba[i * 4 + 3] = px[i] >> 24;
		}
		stbi_write_png(argv[2], W, H, 4, rgba.data(), W * 4);
	}
	printf("frame-test: %s\n", fails ? "failed" : "ok");
	return fails ? 1 : 0;
}
